#include "utils.h"
#include "config.h"
#include "recorder.h"
#include <gst/gst.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <iniparser.h>

/* 异步清理录制 Bin 及关联资源 */
gboolean cleanup_recording_async(gpointer user_data) {
    CustomData *data = (CustomData *)user_data;

    GstElement *recording_bin_temp = (GstElement *)g_atomic_pointer_exchange(&data->recording_bin, NULL);

    if (!recording_bin_temp) {
        g_atomic_int_set(&data->is_recording, FALSE);
        g_atomic_int_set(&data->is_stopping_recording, FALSE);
        return G_SOURCE_REMOVE; 
    }

#ifdef DEBUG
    g_print("Executing asynchronous recording cleanup...\n");
#endif

    // 1. 将整个 Bin 状态设为 NULL 并从父管道安全解绑
    gst_element_set_state(recording_bin_temp, GST_STATE_NULL);

    if (data->pipeline) {
        g_autoptr(GstObject) parent = gst_object_get_parent(GST_OBJECT(recording_bin_temp));
        if (parent == GST_OBJECT(data->pipeline)) {
            gst_bin_remove(GST_BIN(data->pipeline), recording_bin_temp);
        } else {
            gst_object_unref(recording_bin_temp);
        }
    } else {
        gst_object_unref(recording_bin_temp);
    }

    // --- 2. 清理其他标志和字符串 ---
    if (data->recording_filename) {
        g_free(data->recording_filename);
        data->recording_filename = NULL;
    }

    g_atomic_int_set(&data->is_recording, FALSE);
    g_atomic_int_set(&data->is_stopping_recording, FALSE);
#ifdef DEBUG
    g_print("Async recording cleanup complete. Recording stopped.\n");
#endif
    if (data->dialog) {
        gtk_widget_destroy(data->dialog);
        data->dialog = NULL;
    }
    return G_SOURCE_REMOVE;
}

// 探针回调函数：在数据流被安全阻塞时执行 EOS 发送与 Pad 断开操作
static GstPadProbeReturn unlink_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
    (void)info;
    CustomData *data = (CustomData *)user_data;

    // 获取当前阻塞的 Pad 连接的下游对端 Pad (即 recording_bin 的输入端)
    g_autoptr(GstPad) peer = gst_pad_get_peer(pad);

    if (peer) {
        // 1. 向录制分支安全发送 EOS 事件，让复用器(Muxer)正常闭合文件尾部
        gst_pad_send_event(peer, gst_event_new_eos());
        // 2. 断开 Tee 与录制分支的连接
        gst_pad_unlink(pad, peer);
    }

    // 3. 释放 Tee 上的 Request Pad
    if (data->video_tee && pad == data->video_tee_q_pad) {
        gst_element_release_request_pad(data->video_tee, pad);
        data->video_tee_q_pad = NULL;
#ifdef DEBUG
        g_print("Video tee pad safely unlinked and released.\n");
#endif
    } else if (data->audio_tee && pad == data->audio_tee_q_pad) {
        gst_element_release_request_pad(data->audio_tee, pad);
        data->audio_tee_q_pad = NULL;
#ifdef DEBUG
        g_print("Audio tee pad safely unlinked and released.\n");
#endif
    }

    // 4. 移除探针，允许主分支的数据流继续正常流动
    return GST_PAD_PROBE_REMOVE;
}

// 停止录制处理逻辑
gboolean stop_recording(CustomData *data) {
    if (!g_atomic_int_get(&data->is_recording) || !data->pipeline || !data->recording_bin) {
#ifdef DEBUG
        g_print("Recording is not active or missing essential elements.\n");
#endif
        return FALSE;
    }

    g_print("Stopping recording...\n");
    g_atomic_int_set(&data->is_stopping_recording, TRUE);

    // 为视频 Tee 的分支 Pad 添加向下游阻塞的探针
    if (data->video_tee_q_pad) {
        gst_pad_add_probe(data->video_tee_q_pad, 
                          GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, 
                          unlink_cb, 
                          data, 
                          NULL);
    }

    // 为音频 Tee 的分支 Pad 添加向下游阻塞的探针
    if (data->audio_tee_q_pad) {
        gst_pad_add_probe(data->audio_tee_q_pad, 
                          GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, 
                          unlink_cb, 
                          data, 
                          NULL);
    }

    return TRUE;
}

// 启动录制构建与连接
gboolean start_recording(CustomData *data) {
    if (!data->video_tee || !data->audio_tee || !data->pipeline || 
        g_atomic_int_get(&data->is_recording) || !data->config_dict) {
        g_printerr("Recording preconditions failed.\n");
        return FALSE;
    }

    g_print("Starting recording...\n");
    dictionary *dict = data->config_dict;
    GstElement *muxer, *filesink, *video_record_queue, *video_encoder, *video_parser, *audio_record_queue, *audio_encoder;

    // --- 1. 获取 INI 配置参数 ---
    const char *record_path = iniparser_getstring(dict, "main:record_path", "/tmp");
    const char *video_encoder_name = iniparser_getstring(dict, "main:encoder", "x264enc");
    const char *audio_encoder_name = "fdkaacenc";
    const char *video_parser_name = NULL;
    const char *muxer_name = "mp4mux";
    const char *extension = ".mp4";
    g_autofree char *filename_with_ext = NULL;

    if (strstr(video_encoder_name, "h264") != NULL || strstr(video_encoder_name, "x264") != NULL) {
        video_parser_name = "h264parse";
    } else if (strstr(video_encoder_name, "h265") != NULL || strstr(video_encoder_name, "x265") != NULL) {
        video_parser_name = "h265parse";
    } else if (strstr(video_encoder_name, "vp9") != NULL) {
        video_parser_name = "vp9parse";
        audio_encoder_name = "opusenc";
        muxer_name = "webmmux";
        extension = ".webm";
    } else {
        g_printerr("Warning: Unknown encoder %s. Defaulting to h264parse, this might fail.\n", video_encoder_name);
        video_parser_name = "h264parse";
    }

    // --- 2. 创建录制 Bin ---
    data->recording_bin = gst_bin_new("recording-bin");
    if (!data->recording_bin) {
        g_printerr("Failed to create recording bin.\n");
        goto cleanup;
    }
    g_object_set(G_OBJECT(data->recording_bin), "message-forward", TRUE, NULL);

    // 在 Bin 内部创建所有元素
    video_record_queue = gst_element_factory_make("queue", "record-video-queue");
    video_encoder        = gst_element_factory_make(video_encoder_name, "record-video-encoder");
    video_parser         = gst_element_factory_make(video_parser_name, "record-video-parser");
    audio_record_queue = gst_element_factory_make("queue", "record-audio-queue");
    audio_encoder        = gst_element_factory_make(audio_encoder_name, "record-audio-encoder");
    muxer                = gst_element_factory_make(muxer_name, "record-muxer");
    filesink             = gst_element_factory_make("filesink", "record-filesink");

    if (!video_record_queue || !video_encoder || !video_parser || !audio_record_queue || !audio_encoder || !muxer || !filesink) {
        g_printerr("One or more recording elements could not be created.\n");
        goto cleanup;
    }

    // 将所有新元素添加到 bin 中
    gst_bin_add_many(GST_BIN(data->recording_bin), 
                     video_record_queue, video_encoder, video_parser, 
                     audio_record_queue, audio_encoder, muxer, filesink, NULL);

    // --- 3. 配置元素参数 ---
    configure_element_from_ini(video_record_queue, dict, "queue_record");
    configure_element_from_ini(audio_record_queue, dict, "queue_record");
    configure_element_from_ini(video_encoder, dict, video_encoder_name);
    configure_element_from_ini(audio_encoder, dict, audio_encoder_name);
    configure_element_from_ini(muxer, dict, muxer_name);

    // 尝试创建目录（包括父目录）
    if (g_mkdir_with_parents(record_path, 0755) == -1 && errno != EEXIST) {
        g_printerr("Failed to create recording directory: %s\n", record_path);
        goto cleanup;
    }

    // 生成当前时间戳 YYYYMMDD-HHmmss
    time_t rawtime;
    struct tm *info;
    char timestamp[80];
    time(&rawtime);
    info = localtime(&rawtime);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", info);
    filename_with_ext = g_strdup_printf("%s%s", timestamp, extension);
    data->recording_filename = g_build_filename(record_path, filename_with_ext, NULL);

    g_print("Saving recording to: %s\n", data->recording_filename);
    g_object_set(G_OBJECT(filesink), "location", data->recording_filename, NULL);

    // --- 4. 链接 Bin 内部的元素 ---
    if (!gst_element_link_many(video_record_queue, video_encoder, video_parser, muxer, NULL) ||
        !gst_element_link_many(audio_record_queue, audio_encoder, muxer, filesink, NULL)) {
        g_printerr("Failed to link recording elements inside the bin.\n");
        goto cleanup;
    }

    {
        // --- 5. 为 Bin 创建幽灵垫 (Ghost Pads) 作为输入接口 ---
        g_autoptr(GstPad) v_queue_sink_pad = gst_element_get_static_pad(video_record_queue, "sink");
        g_autoptr(GstPad) a_queue_sink_pad = gst_element_get_static_pad(audio_record_queue, "sink");

        if (!v_queue_sink_pad || !a_queue_sink_pad) {
            g_printerr("Failed to get sink pads for queues.\n");
            goto end_of_scope0;
        }

        gst_element_add_pad(data->recording_bin, gst_ghost_pad_new("videosink", g_steal_pointer(&v_queue_sink_pad)));
        gst_element_add_pad(data->recording_bin, gst_ghost_pad_new("audiosink", g_steal_pointer(&a_queue_sink_pad)));

    end_of_scope0:;
    }

    // --- 6. 将整个 Bin 添加到主 Pipeline ---
    gst_bin_add(GST_BIN(data->pipeline), data->recording_bin);

    // 在链接之前，将 bin 状态同步到 PAUSED （可选，但有助于 preroll）
    gst_element_set_state(data->recording_bin, GST_STATE_PAUSED);

    {
        // --- 7. 动态链接主 Pipeline 的 Tee 到 Bin 的幽灵垫 ---
        g_autoptr(GstPad) v_tee_src_pad = gst_element_request_pad_simple(data->video_tee, "src_%u"); 
        g_autoptr(GstPad) a_tee_src_pad = gst_element_request_pad_simple(data->audio_tee, "src_%u");

        g_autoptr(GstPad) v_bin_sink_pad = gst_element_get_static_pad(data->recording_bin, "videosink");
        g_autoptr(GstPad) a_bin_sink_pad = gst_element_get_static_pad(data->recording_bin, "audiosink");

        if (!v_tee_src_pad || !v_bin_sink_pad || !a_tee_src_pad || !a_bin_sink_pad ||
            gst_pad_link(v_tee_src_pad, v_bin_sink_pad) != GST_PAD_LINK_OK ||
            gst_pad_link(a_tee_src_pad, a_bin_sink_pad) != GST_PAD_LINK_OK) {
            g_printerr("Failed to dynamically link tees to recording bin.\n");
            // 链接失败，需要记录 pad 引用以便在 cleanup 释放
            data->video_tee_q_pad = g_steal_pointer(&v_tee_src_pad);
            data->audio_tee_q_pad = g_steal_pointer(&a_tee_src_pad);
            goto end_of_scope1;
        }

        // 将请求到的 tee pad 存储在 CustomData 中，以便在 stop_recording 时释放
        data->video_tee_q_pad = g_steal_pointer(&v_tee_src_pad);
        data->audio_tee_q_pad = g_steal_pointer(&a_tee_src_pad);

        // --- 8. 将 Bin 状态同步到父容器的 PLAYING 状态 ---
        gst_element_sync_state_with_parent(data->recording_bin);

#ifdef DEBUG
        g_print("Recording pipeline linked successfully.\n");
#endif
        g_print("Recording started.\n");
        g_atomic_int_set(&data->is_recording, TRUE);
        return TRUE;
    end_of_scope1:;
    }

cleanup:
    g_printerr("Failed to start recording. Cleaning up.\n");

    if (data->recording_bin) {
        gst_element_set_state(data->recording_bin, GST_STATE_NULL);
        gst_object_unref(data->recording_bin);
        data->recording_bin = NULL;
    }

    g_idle_add(cleanup_recording_async, data);

    return FALSE;
}
