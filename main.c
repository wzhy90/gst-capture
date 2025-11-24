#include <gtk/gtk.h>
#include <gst/gst.h>
#include <stdlib.h>

#include <time.h>
#include <stdio.h>

#include "config.h"

#define CONFIG_FILE "config.ini"

// 全局变量来跟踪动态 Pads（CustomData 结构体中已经有 tee 的指针）
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

/* 辅助函数：用于安全地向管道发送 EOS 事件，启动退出流程 */
static gboolean send_eos_and_quit (gpointer user_data) {
  CustomData *data = (CustomData *)user_data;
  if (!data) {
      gtk_main_quit();
      return G_SOURCE_REMOVE;
  }
#ifdef DEBUG
  g_print("Sending EOS event to the pipeline.\n");
#endif
  if (data->pipeline) {
      // 发送 EOS 事件。管道将处理完剩余数据并最终发送 EOS 消息到总线
      gst_element_send_event(data->pipeline, gst_event_new_eos());
  } else {
      // 如果管道不存在，直接退出 GTK 主循环
      gtk_main_quit();
  }
  return G_SOURCE_REMOVE;
}

/* 主窗口关闭回调 */
static gboolean delete_event_cb (GtkWidget *widget, GdkEvent *event, CustomData *data) {
  // 当窗口关闭时，调用发送 EOS 的函数
  send_eos_and_quit(data);
  return TRUE;
}

/* 全屏切换逻辑的实现函数 */
static void toggle_fullscreen(CustomData *data) {
    static gboolean is_fullscreen = FALSE;
    if (is_fullscreen) {
        gtk_window_unfullscreen (GTK_WINDOW (data->main_window));
    } else {
        gtk_window_fullscreen (GTK_WINDOW (data->main_window));
    }
    is_fullscreen = !is_fullscreen;
}

/* 按钮点击回调函数 */
static void fullscreen_button_cb (GtkButton *button, CustomData *data) {
    toggle_fullscreen(data);
}

/* 辅助函数：负责清理 GStreamer 录制分支（在非 GUI 线程或空闲时调用） */
static gboolean cleanup_recording_branch(gpointer user_data) {
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
            // gst_bin_remove handles unref due to parent change, no extra unref needed here
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
static gboolean stop_recording(CustomData *data) {
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
    // 取消链接本身是线程安全的，但状态改变必须小心
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
static gboolean start_recording(CustomData *data) {
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

    configure_element_from_ini(video_record_queue, dict, "queue");

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

    configure_element_from_ini(audio_record_queue, dict, "queue");

    if (!audio_record_queue || !audio_encoder) {
        g_printerr("Failed to create audio encoder element.\n");
        success = FALSE;
        goto cleanup;
    }

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

/* 录制按钮点击回调函数 */
static void record_button_cb (GtkButton *button, CustomData *data) {
    if (data->is_recording) {
        stop_recording(data);
        gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_image(button)), "media-record-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR); // 停止时显示“录制”图标
    } else {
        start_recording(data);
        if (data->is_recording) {
            gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_image(button)), "media-playback-stop-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR); // 录制时显示“停止”图标
        }
    }
}

/* 键盘事件回调函数 */
static gboolean key_press_event_cb (GtkWidget *widget, GdkEvent *event, CustomData *data) {
  guint keyval;
  gdk_event_get_keyval(event, &keyval);

  switch (keyval) {
    case GDK_KEY_Escape:
    case GDK_KEY_q:
    case GDK_KEY_Q:
      /* 按下 Esc 或 Q 键发送 EOS 退出程序 */
      g_idle_add(send_eos_and_quit, data);
      return TRUE;
    case GDK_KEY_f:
    case GDK_KEY_F:
      /* 按下 F 键切换全屏模式 */
      toggle_fullscreen(data);
      return TRUE;
    default:
      break;
  }
  return FALSE;
}

/* 创建UI组件并注册回调 */
static void create_ui (CustomData *data) {
  GtkWidget *main_box;     /* 主容器 */
  GtkWidget *header_bar;   /* 标题栏 */
  GtkWidget *record_button;     /* 录制按钮 */
  GtkWidget *fullscreen_button; /* 全屏按钮 */

  data->main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  g_signal_connect (G_OBJECT (data->main_window), "delete-event", G_CALLBACK (delete_event_cb), data);
  /* 连接键盘事件回调到主窗口 */
  g_signal_connect (G_OBJECT (data->main_window), "key-press-event", G_CALLBACK (key_press_event_cb), data);

  const char* icon_path = iniparser_getstring(data->config_dict, "main:icon", "app_icon.svg");
  if (icon_path) {
      gtk_window_set_icon_from_file(GTK_WINDOW(data->main_window), icon_path, NULL);
  }

  /* 创建 GtkHeaderBar */
  header_bar = gtk_header_bar_new();
  gtk_header_bar_set_title(GTK_HEADER_BAR(header_bar), "Nintendo Switch");
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header_bar), TRUE);

  /* 创建全屏按钮，使用一个图标 */
  fullscreen_button = gtk_button_new_from_icon_name("view-fullscreen-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (fullscreen_button), "clicked", G_CALLBACK (fullscreen_button_cb), data);

  /* 将按钮打包到 header bar 的末尾（右侧） */
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), fullscreen_button);

  /* 创建录制按钮，使用一个图标 */
  record_button = gtk_button_new_from_icon_name("media-record-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (record_button), "clicked", G_CALLBACK (record_button_cb), data);

  /* 将新按钮打包到 header bar 的末尾 */
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), record_button);

  /* 将 HeaderBar 设置为窗口的标题栏 */
  gtk_window_set_titlebar(GTK_WINDOW(data->main_window), header_bar);

  /* 主布局 (垂直排列，只包含视频区域，HeaderBar由gtk_window_set_titlebar管理) */
  main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  // 直接将视频 sink widget 加入主 box
  gtk_box_pack_start (GTK_BOX (main_box), data->sink_widget, TRUE, TRUE, 0); 

  gtk_container_add (GTK_CONTAINER (data->main_window), main_box);
  gtk_window_set_default_size (GTK_WINDOW (data->main_window), 1280, 720);
  gtk_window_set_position (GTK_WINDOW (data->main_window), GTK_WIN_POS_CENTER);

  gtk_widget_show_all (data->main_window);
}

/* 错误处理回调 */
static void error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;

  /* Print error details on the screen */
  gst_message_parse_error (msg, &err, &debug_info);
  g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
  g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error (&err);
  g_free (debug_info);

  // 发生错误时，同样发送 EOS 信号，以便安全退出
  send_eos_and_quit(data); 
}

/* 流结束回调 */
static void eos_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  //g_print ("End-Of-Stream reached. Quitting application safely.\n");
  // 收到 EOS 消息后，设置管道状态为 NULL 释放资源，然后退出 GTK 主循环
  if (data->pipeline) {
      stop_recording(data);
      gst_element_set_state(data->pipeline, GST_STATE_NULL);
      gst_object_unref(data->pipeline);
      data->pipeline = NULL;
  }
  gtk_main_quit();
}

int main(int argc, char *argv[]) {
  CustomData data = {0};
  GstStateChangeReturn ret;
  GstBus *bus;

  //g_setenv("GST_DEBUG", "WARN", TRUE); 

  /* 初始化GTK */
  gtk_init (&argc, &argv);

  /* 初始化GStreamer */
  gst_init (&argc, &argv);

  /* --- 解析 INI 配置文件 --- */
  data.config_dict = iniparser_load(CONFIG_FILE);
  if (!data.config_dict) {
      g_printerr("Fatal error: Could not open or parse configuration file %s\n", CONFIG_FILE);
      return -1;
  }

  /* 调用 config.c 中的函数来构建管道 */
  if (!initialize_gstreamer_pipeline(&data)) {
      g_printerr("Failed to initialize GStreamer pipeline. Exiting.\n");
      if (data.pipeline) {
          gst_element_set_state(data.pipeline, GST_STATE_NULL);
          gst_object_unref(data.pipeline);
      }
      iniparser_freedict(data.config_dict); // 使用 iniparser_freedict 释放资源
      return -1;
  }

  /* 创建UI (需要在链接元素和获取widget之后调用) */
  create_ui (&data);

  /* 配置消息总线 */
  bus = gst_element_get_bus (data.pipeline);
  gst_bus_add_signal_watch (bus);
  // 连接错误和 EOS 回调
  g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)error_cb, &data);
  g_signal_connect (G_OBJECT (bus), "message::eos", (GCallback)eos_cb, &data);
  gst_object_unref (bus);

  /* 开始播放 */
  ret = gst_element_set_state (data.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    // 启动失败时，直接释放资源并退出
    gst_element_set_state (data.pipeline, GST_STATE_NULL);
    gst_object_unref (data.pipeline);
    return -1;
  }

  /* 启动GTK主循环 */
  gtk_main ();

  if (data.config_dict) {
      iniparser_freedict(data.config_dict);
      data.config_dict = NULL;
  }

  // 确保 GStreamer 管道被释放 (即使在 eos_cb 中被释放，这里做个双保险)
  if (data.pipeline) {
      stop_recording(&data);
      gst_element_set_state(data.pipeline, GST_STATE_NULL);
      gst_object_unref(data.pipeline);
      data.pipeline = NULL;
  }

  return 0;
}

