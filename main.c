#include <gtk/gtk.h>
#include <gst/gst.h>
#include <stdlib.h>

#include <glib-unix.h>

#include <time.h>
#include <stdio.h>

#include "utils.h"
#include "config.h"
#include "recorder.h"

#define CONFIG_FILE "config.ini"

static void create_ui (CustomData *data);
static gboolean on_bus_message(GstBus *bus, GstMessage *msg, CustomData *data);


/* 辅助函数：清理所有应用程序数据和 GStreamer 资源 */
static void cleanup_application_data(CustomData *data) {
#ifdef DEBUG
    g_print("Cleaning up application resources.\n");
#endif
    if (data->app && data->inhibit_cookie > 0) {
        gtk_application_uninhibit(data->app, data->inhibit_cookie);
        data->inhibit_cookie = 0;
#ifdef DEBUG
        g_print("System inhibit request removed.\n");
#endif
    }

    if (data->fps_timer_id > 0) {
        g_source_remove(data->fps_timer_id);
        data->fps_timer_id = 0;
    }

    if (data->config_dict) {
        iniparser_freedict(data->config_dict);
        data->config_dict = NULL;
    }

    data->record_icon = NULL; 

    g_autoptr(GstElement) recording_bin_temp = g_atomic_pointer_exchange(&data->recording_bin, NULL);
    if (recording_bin_temp) {
        gst_element_set_state(recording_bin_temp, GST_STATE_NULL);
    }

    if (data->recording_filename) {
        g_free(data->recording_filename);
        data->recording_filename = NULL;
    }

    g_autoptr(GstElement) pipeline_temp = g_atomic_pointer_exchange(&data->pipeline, NULL);
    if (pipeline_temp) {
        gst_element_set_state(pipeline_temp, GST_STATE_NULL);
    }
}

/* 辅助函数：用于安全地向管道发送 EOS 事件，启动退出流程 */
static gboolean send_eos_and_quit (gpointer user_data) {
  CustomData *data = (CustomData *)user_data;
  if (!data) {
      g_application_quit(G_APPLICATION(data->app));
      return G_SOURCE_REMOVE;
  }
#ifdef DEBUG
  g_print("Sending EOS event to the pipeline.\n");
#endif

  if (data->is_recording) {
#ifdef DEBUG
      g_print("Recording active during quit request, initiating graceful stop.\n");
#endif
      stop_recording(data);
      data->dialog = gtk_message_dialog_new(GTK_WINDOW(data->main_window),
                                                 GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 GTK_MESSAGE_INFO,
                                                 GTK_BUTTONS_NONE,
                                                 "正在停止录制，请稍候...");
      gtk_window_set_title(GTK_WINDOW(data->dialog), "退出程序");
      gtk_dialog_run(GTK_DIALOG(data->dialog));
  }

  if (data->pipeline) {
      gst_element_send_event(data->pipeline, gst_event_new_eos());
  } else {
      g_application_quit(G_APPLICATION(data->app));
  }
  return G_SOURCE_REMOVE;
}

static gboolean on_delete_event(GtkWidget *widget, GdkEvent *event, CustomData *data) {
    g_idle_add(send_eos_and_quit, data);
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

/* 录制按钮点击回调函数 */
static void record_button_cb (GtkButton *button, CustomData *data) {
    if (data->is_stopping_recording) {
#ifdef DEBUG
        g_print("Recording is currently stopping/cleaning up. Please wait.\n");
#endif
        return;
    }
    if (data->is_recording) {
        stop_recording(data);
        gtk_image_set_from_icon_name(GTK_IMAGE(data->record_icon), "media-record-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
    } else {
        start_recording(data);
        if (data->is_recording) {
            gtk_image_set_from_icon_name(GTK_IMAGE(data->record_icon), "media-playback-stop-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
        }
    }
}

/* 定时器回调函数：每秒执行一次，从 stats 属性中抓取真实帧率 */
static gboolean refresh_fps_status_cb(gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    if (!data->gtkglsink || !data->fps_label) return G_SOURCE_CONTINUE;

    GstStructure *stats = NULL;
    g_object_get(data->gtkglsink, "stats", &stats, NULL);

    if (stats) {
        guint64 current_rendered = 0;
        // 1. 抓取底層自開機以來「總共成功渲染的畫格數」
        if (gst_structure_get_uint64(stats, "rendered", &current_rendered)) {
            gint64 current_time = g_get_monotonic_time(); // 獲取當前微秒級時間

            // 2. 如果不是第一次執行，計算與上一秒的差值
            if (data->last_fps_time > 0) {
                double time_diff_sec = (double)(current_time - data->last_fps_time) / 1000000.0;
                guint64 frames_diff = current_rendered - data->last_rendered_count;

                // 計算瞬時幀率 = 幀數差 / 時間差
                double instant_fps = (double)frames_diff / time_diff_sec;

                // 防止極端情況下（比如剛切換全螢幕時時間抖動）數值異常
                if (instant_fps < 0.0) instant_fps = 0.0;

                // 3. 漂亮地顯示在左上角（直角 24px 等寬字體）
                gchar *fps_text = g_strdup_printf("%.2f", instant_fps);
                gtk_label_set_text(GTK_LABEL(data->fps_label), fps_text);
                gtk_widget_show(data->fps_label);
                g_free(fps_text);
            }

            // 4. 更新歷史紀錄，供下一秒計算使用
            data->last_rendered_count = current_rendered;
            data->last_fps_time = current_time;
        }
        gst_structure_free(stats);
    }
    return G_SOURCE_CONTINUE;
}

/* 帧率切换按钮点击回呼函数 */
static void fps_button_cb (GtkToggleButton *button, CustomData *data) {
    gboolean show_fps = gtk_toggle_button_get_active(button);

    if (show_fps) {
        // 每次打開開關時，重設時間戳和計數器
        data->last_rendered_count = 0;
        data->last_fps_time = 0;

        if (data->fps_timer_id == 0) {
            data->fps_timer_id = g_timeout_add_full(G_PRIORITY_HIGH, 500, refresh_fps_status_cb, data, NULL);
        }
        refresh_fps_status_cb(data);
    } else {
        if (data->fps_timer_id != 0) {
            g_source_remove(data->fps_timer_id);
            data->fps_timer_id = 0;
        }
        gtk_label_set_text(GTK_LABEL(data->fps_label), "");
        gtk_widget_hide(data->fps_label); 
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
  GtkWidget *overlay;
  GtkWidget *header_bar;   /* 标题栏 */
  GtkWidget *record_button;     /* 录制按钮 */
  GtkWidget *fullscreen_button; /* 全屏按钮 */
  GtkWidget *fps_button;        /* 帧率开关按钮 */

  data->main_window = gtk_application_window_new (data->app);
  g_signal_connect (G_OBJECT (data->main_window), "delete-event", G_CALLBACK (on_delete_event), data);
  g_signal_connect (G_OBJECT (data->main_window), "key-press-event", G_CALLBACK (key_press_event_cb), data);

  const char* icon_path = iniparser_getstring(data->config_dict, "main:icon", "app_icon.svg");
  if (icon_path) {
      gtk_window_set_icon_from_file(GTK_WINDOW(data->main_window), icon_path, NULL);
  }

  /* 创建 GtkHeaderBar */
  header_bar = gtk_header_bar_new();
  const char* win_title = iniparser_getstring(data->config_dict, "main:win_title", "gst-capture");
  if (win_title) {
      gtk_header_bar_set_title(GTK_HEADER_BAR(header_bar), win_title);
  }
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header_bar), TRUE);

  /* 创建全屏按钮，使用一个图标 */
  fullscreen_button = gtk_button_new_from_icon_name("view-fullscreen-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect (G_OBJECT (fullscreen_button), "clicked", G_CALLBACK (fullscreen_button_cb), data);
  /* 将按钮打包到 header bar 的末尾（右侧） */
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), fullscreen_button);

  /* 创建控制 FPS 显示的开关按钮 */
  fps_button = gtk_toggle_button_new_with_label("FPS");
  g_signal_connect (G_OBJECT (fps_button), "toggled", G_CALLBACK (fps_button_cb), data);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), fps_button);

  // 根据 has_tee 决定是否显示录制按钮
  if (data->has_tee) {
    /* 创建录制按钮，使用一个图标 */
    record_button = gtk_button_new_from_icon_name("media-record-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
    g_signal_connect (G_OBJECT (record_button), "clicked", G_CALLBACK (record_button_cb), data);
    data->record_icon = gtk_button_get_image(GTK_BUTTON(record_button));
    /* 将新按钮打包到 header bar 的末尾 */
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header_bar), record_button);
  }

  /* 将 HeaderBar 设置为窗口的标题栏 */
  gtk_window_set_titlebar(GTK_WINDOW(data->main_window), header_bar);

  /* 主布局 (垂直排列，只包含视频区域，HeaderBar由gtk_window_set_titlebar管理) */
  main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add (GTK_CONTAINER (data->main_window), main_box);

  /* 2. 創建 Overlay 容器並打包進主容器 */
  overlay = gtk_overlay_new();
  gtk_box_pack_start(GTK_BOX(main_box), overlay, TRUE, TRUE, 0);

  /* 3. 從 GStreamer 獲取視頻渲染組件 (gtkglsink 創建的 widget) */
  if (data->gtkglsink) {
      g_object_get(data->gtkglsink, "widget", &data->sink_widget, NULL);
  } else {
      g_printerr("Error: data->gtkglsink is NULL, cannot get GTK widget!\n");
  }
  
  /* 4. 把視頻組件作為 Overlay 的「底層主畫面」 */
  if (data->sink_widget) {
      gtk_container_add(GTK_CONTAINER(overlay), data->sink_widget);
  }

  /* 5. 創建 FPS 文本標籤並設置右上角懸浮對齊 */
  data->fps_label = gtk_label_new("");
  gtk_widget_set_halign(data->fps_label, GTK_ALIGN_START);
  gtk_widget_set_valign(data->fps_label, GTK_ALIGN_START);
  
  /* 6. 將外邊距完全歸零 */
  gtk_widget_set_margin_top(data->fps_label, 0);
  gtk_widget_set_margin_start(data->fps_label, 0);

  /* 7. 使用 CSS 注入美化懸浮 OSD 文字（黑底綠字，等寬字體） */
  GtkStyleContext *context = gtk_widget_get_style_context(data->fps_label);
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(provider,
      "label {"
      "  color: #00FF00;"               /* 經典綠色數位字體 */
      "  background-color: rgba(0, 0, 0, 0.42);" /* 40% 透明度的黑色背景 */
      "  font-family: 'Roboto Mono', monospace;" /* 等寬字體，防止數字跳動時框框抖動 */
      "  font-size: 24px;"              /* 增大字號，全螢幕也清晰可見 */
      "  font-weight: bold;"
      "  padding-top: 0px; "
      "  padding-bottom: 2px;"
      "  padding-left: 5px;"
      "  padding-right: 5px;"
      "  border-radius: 0px;"           /* 圓角 */
      "}", -1, NULL);
  gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  /* 8. 把 FPS 標籤作為「懸浮層」疊加到視頻畫面上方 */
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), data->fps_label);

  /* ================================================================= */
  /* 視窗尺寸與顯示處理 */
  /* ================================================================= */
  gint width = 1280;
  gint height = 720;
  const char *win_size_str = iniparser_getstring(data->config_dict, "main:win_size", NULL);

  if (win_size_str) {
    if (sscanf(win_size_str, "%dx%d", &width, &height) != 2) {
      width = 1280;
      height = 720;
    }
  }

  gtk_window_set_default_size (GTK_WINDOW (data->main_window), width, height);
  gtk_window_set_position (GTK_WINDOW (data->main_window), GTK_WIN_POS_CENTER);

  /* 必須先 show_all 才能確保組件正常渲染 */
  gtk_widget_show_all (data->main_window);

  if (data->default_show_fps) {
      // 如果 INI 為 1/true/yes，將按鈕設為選中狀態（這會自動觸發 fps_button_cb 定時器）
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fps_button), TRUE);
      gtk_widget_show(data->fps_label);
  } else {
      // 如果 INI 為 0/false/no，按鈕保持未選中，且隱藏標籤
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fps_button), FALSE);
      gtk_widget_hide(data->fps_label);
  }

  data->inhibit_cookie = gtk_application_inhibit(
      data->app,
      GTK_WINDOW(data->main_window),
      GTK_APPLICATION_INHIBIT_SUSPEND | GTK_APPLICATION_INHIBIT_IDLE,
      "Video Playback Active"
  );
}

static gboolean signal_handler(gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
#ifdef DEBUG
    g_print("System signal caught (SIGINT or SIGTERM). Initiating graceful application quit.\n");
#endif
    send_eos_and_quit(data);

    return G_SOURCE_REMOVE; 
}

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, CustomData *data) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            g_autoptr(GError) err = NULL;
            g_autofree gchar *debug_info = NULL;

            gst_message_parse_error(msg, &err, &debug_info);
            g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
            g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");

            cleanup_application_data(data); 
            g_application_quit(G_APPLICATION(data->app));
            break;
        }

        case GST_MESSAGE_EOS: {
#ifdef DEBUG
            g_print("End-Of-Stream reached on main pipeline. Quitting application safely.\n");
#endif
            cleanup_application_data(data);
            g_application_quit(G_APPLICATION(data->app));
            break;
        }

        case GST_MESSAGE_ELEMENT: {
            if (gst_message_has_name(msg, "GstBinForwarded")) {
                const GstStructure *s = gst_message_get_structure(msg);
                const GValue *gv = gst_structure_get_value(s, "message");
                GstMessage *forwarded_msg = NULL;

                if (G_VALUE_HOLDS_BOXED(gv)) {
                    forwarded_msg = (GstMessage *)g_value_get_boxed(gv);
                }

                if (forwarded_msg != NULL && GST_MESSAGE_TYPE(forwarded_msg) == GST_MESSAGE_EOS) {
                    if (data->is_stopping_recording && 
                        GST_ELEMENT_CAST(GST_OBJECT_PARENT(GST_MESSAGE_SRC(forwarded_msg))) == data->recording_bin) {
#ifdef DEBUG
                             g_print("Received forwarded EOS from recording sink. Initiating final cleanup via idle function.\n");
#endif
                             g_idle_add(cleanup_recording_async, data);
                    }
                }
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}

static void on_activate(GtkApplication* app, gpointer user_data) {
    CustomData *data = (CustomData *)user_data;
    data->app = app; // 保存 app 指针到数据结构

    data->config_dict = iniparser_load(CONFIG_FILE);
    if (!data->config_dict) {
        g_printerr("Fatal error: Could not open or parse configuration file %s\n", CONFIG_FILE);
        g_application_quit(G_APPLICATION(app));
        return;
    }

    if (!initialize_gstreamer_pipeline(data)) {
        g_printerr("Failed to initialize GStreamer pipeline. Exiting.\n");
        cleanup_application_data(data);
        g_application_quit(G_APPLICATION(app));
        return;
    }

    create_ui (data);

    g_autoptr(GstBus) bus = gst_element_get_bus (data->pipeline);
    gst_bus_add_signal_watch (bus);
    g_signal_connect (G_OBJECT (bus), "message", (GCallback)on_bus_message, data);

    GstStateChangeReturn ret = gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr ("Unable to set the pipeline to the playing state.\n");
        cleanup_application_data(data);
        g_application_quit(G_APPLICATION(app));
        return;
    }
}

int main(int argc, char *argv[]) {
  CustomData data = {0};
  int status;

  data.app = gtk_application_new("org.gstcapture", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(data.app, "activate", G_CALLBACK(on_activate), &data);

  g_unix_signal_add(SIGINT, signal_handler, &data);
  g_unix_signal_add(SIGTERM, signal_handler, &data);

  gst_init (&argc, &argv);

  status = g_application_run(G_APPLICATION(data.app), argc, argv);

  g_object_unref(data.app);

  return status;
}

