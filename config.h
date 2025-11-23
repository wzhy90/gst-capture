#ifndef CONFIG_H
#define CONFIG_H

#include <gst/gst.h>
#include <gtk/gtk.h>

#include <iniparser/iniparser.h>
#include <iniparser/dictionary.h>

#define DEBUG

/* 结构体包含所有需要传递的信息 (与 main.c 中的定义一致) */
typedef struct _CustomData {
  GstElement *pipeline;          /* 主管道 */
  GstElement *videosink;         /* 视频输出元素 */

  GtkWidget *sink_widget;        /* 视频显示组件 */
  GtkWidget *main_window;        /* 主窗口指针, 用于全屏/退出控制 */
  dictionary *config_dict;                 /* 指向解析后的配置数据的指针 */
} CustomData;

/*
 * 初始化并构建 GStreamer 管道
 * data: 指向 CustomData 结构体的指针
 * 返回值: 如果成功返回 TRUE，失败返回 FALSE
 */
gboolean initialize_gstreamer_pipeline(CustomData *data);

#endif // CONFIG_H
