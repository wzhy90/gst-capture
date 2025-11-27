#include "recorder.h"
#include <gst/gst.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <iniparser.h>

static GstPad *video_tee_q_pad = NULL;
static GstPad *audio_tee_q_pad = NULL;
static GstElement *muxer = NULL;
static GstElement *filesink = NULL;
static GstElement *video_record_queue = NULL;
static GstElement *video_encoder = NULL;
static GstElement *h264_parser = NULL;
static GstElement *audio_record_queue = NULL;
static GstElement *audio_encoder = NULL;
static gchar *recording_filename = NULL;


/* 辅助函数：负责清理 GStreamer 录制分支（在非 GUI 线程或空闲时调用） */
gboolean cleanup_recording_branch(gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    if (!data->is_recording || !data->pipeline) {
        return G_SOURCE_REMOVE;
    }
#ifdef DEBUG
    g_print("Cleaning up recording branch...\n");
#endif
    // 1. 将录制分支的所有元素状态设为 NULL，并从管道中移除
    GstElement *record_elements[] = {
        video_record_queue, video_encoder, h264_parser, 
        audio_record_queue, audio_encoder, 
        muxer, filesink, NULL
    };

    for (int i = 0; record_elements[i] != NULL; i++) {
        if (record_elements[i]) {
            // 必须先设置为 NULL 状态才能安全移除
            gst_element_set_state(record_elements[i], GST_STATE_NULL);
            gst_bin_remove(GST_BIN(data->pipeline), record_elements[i]);
        }
    }

    // 2. 清理全局指针和标志
    muxer = NULL;
    filesink = NULL;
    video_record_queue = NULL;
    video_encoder = NULL;
    h264_parser = NULL;
    audio_record_queue = NULL;
    audio_encoder = NULL;

    if (recording_filename) {
        g_free(recording_filename);
        recording_filename = NULL;
    }

    data->is_recording = FALSE;
#ifdef DEBUG
    g_print("Recording cleanup complete.\n");
#endif
    g_print("Recording stopped.\n");
    return G_SOURCE_REMOVE;
}

// 辅助函数：停止录制并清理分支 (新实现)
gboolean stop_recording(CustomData *data) {
    if (!data->is_recording || !data->pipeline) {
        return FALSE;
    }

    g_print("Stopping recording...\n");

    // 在取消链接之前，向录制分支的 Queue 发送 EOS 事件，确保数据写完
    if (video_record_queue) {
        gst_element_send_event(video_record_queue, gst_event_new_eos());
    }
    if (audio_record_queue) {
        gst_element_send_event(audio_record_queue, gst_event_new_eos());
    }

    // 1. 取消动态链接并释放 Pad
    if (video_tee_q_pad) {
        GstPad *v_queue_sink_pad = gst_element_get_static_pad(video_record_queue, "sink");
        if (v_queue_sink_pad) {
            gst_pad_unlink(video_tee_q_pad, v_queue_sink_pad);
            gst_object_unref(v_queue_sink_pad);
        }
        gst_element_release_request_pad(data->video_tee, video_tee_q_pad);
        gst_object_unref(video_tee_q_pad);
        video_tee_q_pad = NULL;
    }

    if (audio_tee_q_pad) {
        GstPad *a_queue_sink_pad = gst_element_get_static_pad(audio_record_queue, "sink");
        if (a_queue_sink_pad) {
            gst_pad_unlink(audio_tee_q_pad, a_queue_sink_pad);
            gst_object_unref(a_queue_sink_pad);
        }
        gst_element_release_request_pad(data->audio_tee, audio_tee_q_pad);
        gst_object_unref(audio_tee_q_pad);
        audio_tee_q_pad = NULL;
    }

    g_idle_add(cleanup_recording_branch, data);

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

    // --- 1. 获取 INI 配置参数 ---
    const char *record_path = iniparser_getstring(dict, "main:record_path", "/tmp");
    const char *video_encoder_name = iniparser_getstring(dict, "main:encoder", "x264enc");
    const char *audio_encoder_name = "fdkaacenc";
    
    // --- 2. 创建 Muxer (mp4mux) 和 Filesink ---
    muxer = create_and_add_element("mp4mux", "record-muxer", GST_BIN(data->pipeline));
    filesink = create_and_add_element("filesink", "record-filesink", GST_BIN(data->pipeline));

    if (!muxer || !filesink) {
        g_printerr("Failed to create muxer or filesink.\n");
        success = FALSE;
        goto cleanup;
    }

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
    // 格式化时间，例如 "20251124-064100.mp4"
    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S.mp4", info);
    recording_filename = g_build_filename(record_path, timestamp, NULL);

    g_print("Saving recording to: %s\n", recording_filename);
    g_object_set(G_OBJECT(filesink), "location", recording_filename, NULL);

    // 链接 muxer 和 sink
    if (!gst_element_link(muxer, filesink)) {
        g_printerr("Failed to link muxer to filesink.\n");
        success = FALSE;
        goto cleanup;
    }

    // --- 3. 创建并配置视频录制分支元素 (queue -> encoder -> h264parse) ---
    video_record_queue = create_and_add_element("queue", "record-video-queue", GST_BIN(data->pipeline));
    video_encoder = create_and_add_element(video_encoder_name, "record-video-encoder", GST_BIN(data->pipeline));
    h264_parser = create_and_add_element("h264parse", "record-h264-parser", GST_BIN(data->pipeline));

    configure_element_from_ini(video_record_queue, dict, "queue_record");

    if (!video_record_queue || !video_encoder || !h264_parser) {
        g_printerr("Failed to create video encoder element.\n");
        success = FALSE;
        goto cleanup;
    }

    configure_element_from_ini(video_encoder, dict, video_encoder_name);

    // 静态链接视频分支内部
    if (!gst_element_link_many(video_record_queue, video_encoder, h264_parser, muxer, NULL)) {
        g_printerr("Failed to link video recording elements.\n");
        success = FALSE;
        goto cleanup;
    }

    // --- 4. 创建并配置音频录制分支元素 (queue -> fdkaacenc) ---
    audio_record_queue = create_and_add_element("queue", "record-audio-queue", GST_BIN(data->pipeline));
    audio_encoder = create_and_add_element(audio_encoder_name, "record-audio-encoder", GST_BIN(data->pipeline));
    
    // 使用 INI 配置音频 queue
    configure_element_from_ini(audio_record_queue, dict, "queue_record");


    if (!audio_record_queue || !audio_encoder) {
        g_printerr("Failed to create audio encoder element.\n");
        success = FALSE;
        goto cleanup;
    }

    // 硬编码设置 bitrate-mode
    g_object_set(G_OBJECT(audio_encoder), "bitrate-mode", 5, NULL);

    if (!gst_element_link_many(audio_record_queue, audio_encoder, muxer, NULL)) {
         g_printerr("Failed to link audio recording elements.\n");
         success = FALSE;
         goto cleanup;
    }

    // --- 5. 将所有新元素状态同步到 PLAYING ---
    GstElement *elements_to_sync[] = {
        muxer, filesink, 
        video_record_queue, video_encoder, h264_parser, 
        audio_record_queue, audio_encoder, NULL
    };

    for (int i = 0; elements_to_sync[i] != NULL; i++) {
        gst_element_sync_state_with_parent(elements_to_sync[i]);
    }

    // --- 6 & 7. 动态链接 Video Tee 和 Audio Tee 到各自的 Queue ---
    video_tee_q_pad = gst_element_request_pad_simple(data->video_tee, "src_%u"); 
    GstPad *v_queue_sink_pad = gst_element_get_static_pad(video_record_queue, "sink");
    if (!video_tee_q_pad || !v_queue_sink_pad || gst_pad_link(video_tee_q_pad, v_queue_sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Failed to dynamically link video tee.\n");
        if(v_queue_sink_pad) gst_object_unref(v_queue_sink_pad);
        success = FALSE;
        goto cleanup;
    }
    gst_object_unref(v_queue_sink_pad);

    // Audio Tee
    audio_tee_q_pad = gst_element_request_pad_simple(data->audio_tee, "src_%u");
    GstPad *a_queue_sink_pad = gst_element_get_static_pad(audio_record_queue, "sink");
    if (!audio_tee_q_pad || !a_queue_sink_pad || gst_pad_link(audio_tee_q_pad, a_queue_sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Failed to dynamically link audio tee.\n");
        if(a_queue_sink_pad) gst_object_unref(a_queue_sink_pad);
        success = FALSE;
        goto cleanup;
    }
    gst_object_unref(a_queue_sink_pad);

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

