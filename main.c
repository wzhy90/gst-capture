#include <gtk/gtk.h>
#include <gst/gst.h>
#include <stdlib.h>

#include <time.h>
#include <stdio.h>

#include "config.h"
#include "recorder.h"

#define CONFIG_FILE "config.ini"

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

/* 录制按钮点击回调函数 */
static void record_button_cb (GtkButton *button, CustomData *data) {
    if (data->is_recording) {
        stop_recording(data);
        gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_image(button)), "media-record-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
    } else {
        start_recording(data);
        if (data->is_recording) {
            gtk_image_set_from_icon_name(GTK_IMAGE(gtk_button_get_image(button)), "media-playback-stop-symbolic", GTK_ICON_SIZE_SMALL_TOOLBAR);
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
#ifdef DEBUG
  g_print ("End-Of-Stream reached. Quitting application safely.\n");
#endif
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

  gtk_init (&argc, &argv);
  gst_init (&argc, &argv);

  data.config_dict = iniparser_load(CONFIG_FILE);
  if (!data.config_dict) {
      g_printerr("Fatal error: Could not open or parse configuration file %s\n", CONFIG_FILE);
      return -1;
  }

  if (!initialize_gstreamer_pipeline(&data)) {
      g_printerr("Failed to initialize GStreamer pipeline. Exiting.\n");
      if (data.pipeline) {
          gst_element_set_state(data.pipeline, GST_STATE_NULL);
          gst_object_unref(data.pipeline);
      }
      iniparser_freedict(data.config_dict);
      return -1;
  }

  create_ui (&data);

  bus = gst_element_get_bus (data.pipeline);
  gst_bus_add_signal_watch (bus);
  g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)error_cb, &data);
  g_signal_connect (G_OBJECT (bus), "message::eos", (GCallback)eos_cb, &data);
  gst_object_unref (bus);

  ret = gst_element_set_state (data.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr ("Unable to set the pipeline to the playing state.\n");
    stop_recording(&data);
    gst_element_set_state (data.pipeline, GST_STATE_NULL);
    gst_object_unref (data.pipeline);
    iniparser_freedict(data.config_dict);
    return -1;
  }

  gtk_main ();

  if (data.config_dict) {
      iniparser_freedict(data.config_dict);
      data.config_dict = NULL;
  }
  
  if (data.pipeline) {
      stop_recording(&data);
      gst_element_set_state(data.pipeline, GST_STATE_NULL);
      gst_object_unref(data.pipeline);
      data.pipeline = NULL;
  }

  return 0;
}

