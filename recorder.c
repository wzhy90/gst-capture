#include "utils.h"
#include "config.h"
#include "recorder.h"
#include <gst/gst.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <iniparser.h>

static GstPad *video_tee_q_pad = NULL;
static GstPad *audio_tee_q_pad = NULL;

gboolean cleanup_recording_async(gpointer user_data) {
    CustomData *data = (CustomData *)user_data;

    if (!data->recording_bin) {
        data->is_recording = FALSE;
        data->is_stopping_recording = FALSE;
        return G_SOURCE_REMOVE; 
    }
#ifdef DEBUG
    g_print("Executing asynchronous recording cleanup...\n");
#endif
    // --- 1. 将整个 Bin 状态设置为 GST_STATE_NULL ---
    GstStateChangeReturn state_ret = gst_element_set_state(data->recording_bin, GST_STATE_NULL);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
         g_printerr("Async: Failed to set recording bin to NULL state.\n");
    }

    if (data->pipeline && data->recording_bin) {
        gst_bin_remove(GST_BIN(data->pipeline), data->recording_bin);
    }

    data->recording_bin = NULL; 

    // --- 2. 清理其他标志和字符串 ---
    if (data->recording_filename) {
        g_free(data->recording_filename);
        data->recording_filename = NULL;
    }

    data->is_recording = FALSE;
    data->is_stopping_recording = FALSE;
#ifdef DEBUG
    g_print("Async recording cleanup complete. Recording stopped.\n");
#endif
    return G_SOURCE_REMOVE; 
}

// 辅助函数：停止录制并清理分支 (新实现)
gboolean stop_recording(CustomData *data) {
    if (!data->is_recording || !data->pipeline || !data->recording_bin) {
#ifdef DEBUG
        g_print("Recording is not active or missing essential elements.\n");
#endif
        return FALSE;
    }

    g_print("Stopping recording...\n");
    data->is_stopping_recording = TRUE;

    GstPad *v_bin_sink_pad = gst_element_get_static_pad(data->recording_bin, "videosink");
    GstPad *a_bin_sink_pad = gst_element_get_static_pad(data->recording_bin, "audiosink");
    
    if (v_bin_sink_pad) {
        gst_pad_send_event(v_bin_sink_pad, gst_event_new_eos());
        gst_object_unref(v_bin_sink_pad);
    }
    if (a_bin_sink_pad) {
        gst_pad_send_event(a_bin_sink_pad, gst_event_new_eos());
        gst_object_unref(a_bin_sink_pad);
    }
    
    // --- 立即从 Tee 侧取消动态链接并释放 Pad ---
    if (data->video_tee_q_pad && data->video_tee) {
        gst_element_release_request_pad(data->video_tee, data->video_tee_q_pad);
        data->video_tee_q_pad = NULL;
    }

    if (data->audio_tee_q_pad && data->audio_tee) {
        gst_element_release_request_pad(data->audio_tee, data->audio_tee_q_pad);
        data->audio_tee_q_pad = NULL;
    }

    return TRUE;
}

// 辅助函数：构建并链接录制分支
gboolean start_recording(CustomData *data) {
    if (!data->video_tee || !data->audio_tee || !data->pipeline || data->is_recording || !data->config_dict) {
        g_printerr("Recording preconditions failed.\n");
        return FALSE;
    }

    g_print("Starting recording...\n");
    gboolean success = TRUE;
    dictionary *dict = data->config_dict;
    GstElement *muxer, *filesink, *video_record_queue, *video_encoder, *video_parser, *audio_record_queue, *audio_encoder;
    GstPad *v_tee_src_pad = NULL, *a_tee_src_pad = NULL;

    // --- 1. 获取 INI 配置参数 ---
    const char *record_path = iniparser_getstring(dict, "main:record_path", "/tmp");
    const char *video_encoder_name = iniparser_getstring(dict, "main:encoder", "x264enc");
    const char *audio_encoder_name = "fdkaacenc";
    const char *video_parser_name = NULL;

    if (strstr(video_encoder_name, "h264") != NULL || strstr(video_encoder_name, "x264") != NULL) {
        video_parser_name = "h264parse";
    } else if (strstr(video_encoder_name, "h265") != NULL || strstr(video_encoder_name, "x265") != NULL) {
        video_parser_name = "h265parse";
    } else if (strstr(video_encoder_name, "vp9") != NULL) {
        video_parser_name = "vp9parse";
        audio_encoder_name = "opusenc";
    } else {
        g_printerr("Warning: Unknown encoder %s. Defaulting to h264parse, this might fail.\n", video_encoder_name);
        video_parser_name = "h264parse";
    }

    // --- 2. 创建并组装一个 GstBin 作为录制子管道 ---
    data->recording_bin = gst_bin_new("recording-bin");
    if (!data->recording_bin) {
        g_printerr("Failed to create recording bin.\n");
        success = FALSE;
        goto cleanup;
    }
    g_object_set(G_OBJECT(data->recording_bin), "message-forward", TRUE, NULL);

    // 在 Bin 内部创建所有元素
    video_record_queue = gst_element_factory_make("queue", "record-video-queue");
    video_encoder        = gst_element_factory_make(video_encoder_name, "record-video-encoder");
    video_parser         = gst_element_factory_make(video_parser_name, "record-video-parser");
    audio_record_queue = gst_element_factory_make("queue", "record-audio-queue");
    audio_encoder        = gst_element_factory_make(audio_encoder_name, "record-audio-encoder");
    muxer                = gst_element_factory_make("mp4mux", "record-muxer");
    filesink             = gst_element_factory_make("filesink", "record-filesink");

    if (!video_record_queue || !video_encoder || !video_parser || !audio_record_queue || !audio_encoder || !muxer || !filesink) {
        g_printerr("One or more recording elements could not be created.\n");
        success = FALSE;
        goto cleanup;
    }

    // 将所有新元素添加到 bin 中
    gst_bin_add_many(GST_BIN(data->recording_bin), 
                     video_record_queue, video_encoder, video_parser, 
                     audio_record_queue, audio_encoder, muxer, filesink, NULL);

    // --- 3. 配置元素 ---
    configure_element_from_ini(video_record_queue, dict, "queue_record");
    configure_element_from_ini(audio_record_queue, dict, "queue_record");
    configure_element_from_ini(video_encoder, dict, video_encoder_name);
    configure_element_from_ini(audio_encoder, dict, audio_encoder_name);
    configure_element_from_ini(muxer, dict, "mp4mux");

    // 尝试创建目录（包括父目录）
    if (g_mkdir_with_parents(record_path, 0755) == -1 && errno != EEXIST) {
        g_printerr("Failed to create recording directory: %s\n", record_path);
        success = FALSE;
        goto cleanup;
    }

    // 生成当前时间戳 YYYYMMDD-HHmmss
    time_t rawtime;
    struct tm *info;
    char timestamp[80];
    time(&rawtime);
    info = localtime(&rawtime);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S.mp4", info);
    data->recording_filename = g_build_filename(record_path, timestamp, NULL);

    g_print("Saving recording to: %s\n", data->recording_filename);
    g_object_set(G_OBJECT(filesink), "location", data->recording_filename, NULL);

    // --- 4. 链接 Bin 内部的元素 ---
    if (!gst_element_link_many(video_record_queue, video_encoder, video_parser, muxer, NULL) ||
        !gst_element_link_many(audio_record_queue, audio_encoder, muxer, filesink, NULL)) { // filesink 直接连到 muxer
        g_printerr("Failed to link recording elements inside the bin.\n");
        success = FALSE;
        goto cleanup;
    }

    // --- 5. 为 Bin 创建幽灵垫 (Ghost Pads) 作为输入接口 ---
    GstPad *v_queue_sink_pad = gst_element_get_static_pad(video_record_queue, "sink");
    GstPad *a_queue_sink_pad = gst_element_get_static_pad(audio_record_queue, "sink");
    
    gst_element_add_pad(data->recording_bin, gst_ghost_pad_new("videosink", v_queue_sink_pad));
    gst_element_add_pad(data->recording_bin, gst_ghost_pad_new("audiosink", a_queue_sink_pad));
    
    // 幽灵垫创建后，原始 pad 的引用计数由 bin 管理，我们可以 unref 掉局部引用
    gst_object_unref(v_queue_sink_pad);
    gst_object_unref(a_queue_sink_pad);

    // --- 6. 将整个 Bin 添加到主 Pipeline ---
    gst_bin_add(GST_BIN(data->pipeline), data->recording_bin);

    // 在链接之前，将 bin 状态同步到 PAUSED （可选，但有助于 preroll）
    gst_element_set_state(data->recording_bin, GST_STATE_PAUSED);

    // --- 7. 动态链接主 Pipeline 的 Tee 到 Bin 的幽灵垫 ---
    v_tee_src_pad = gst_element_request_pad_simple(data->video_tee, "src_%u"); 
    a_tee_src_pad = gst_element_request_pad_simple(data->audio_tee, "src_%u");

    GstPad *v_bin_sink_pad = gst_element_get_static_pad(data->recording_bin, "videosink");
    GstPad *a_bin_sink_pad = gst_element_get_static_pad(data->recording_bin, "audiosink");

    if (!v_tee_src_pad || !v_bin_sink_pad || !a_tee_src_pad || !a_bin_sink_pad ||
        gst_pad_link(v_tee_src_pad, v_bin_sink_pad) != GST_PAD_LINK_OK ||
        gst_pad_link(a_tee_src_pad, a_bin_sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Failed to dynamically link tees to recording bin.\n");
        // 链接失败，需要记录 pad 引用以便在 cleanup 释放
        data->video_tee_q_pad = v_tee_src_pad; // 存起来以便 cleanup 释放请求的 pad
        data->audio_tee_q_pad = a_tee_src_pad;
        if(v_bin_sink_pad) gst_object_unref(v_bin_sink_pad);
        if(a_bin_sink_pad) gst_object_unref(a_bin_sink_pad);
        success = FALSE;
        goto cleanup;
    }

    // 将请求到的 tee pad 存储在 CustomData 中，以便在 stop_recording 时释放
    data->video_tee_q_pad = v_tee_src_pad;
    data->audio_tee_q_pad = a_tee_src_pad;
    
    // 释放获取的 bin sink pad 引用
    gst_object_unref(v_bin_sink_pad);
    gst_object_unref(a_bin_sink_pad);

    // --- 8. 将 Bin 状态同步到父容器的 PLAYING 状态 ---
    // 这将启动整个录制分支
    gst_element_sync_state_with_parent(data->recording_bin);

#ifdef DEBUG
    g_print("Recording pipeline linked successfully.\n");
#endif
    g_print("Recording started.\n");
    data->is_recording = TRUE;
    return TRUE;

cleanup:
    g_printerr("Failed to start recording. Cleaning up.\n");
    stop_recording(data); 
    return FALSE;
}

