#ifndef CONFIG_H
#define CONFIG_H

#include "utils.h"
#include <gst/gst.h>
#include <gtk/gtk.h>

/* 结构体包含所有需要传递的信息 (与 main.c 中的定义一致) */
typedef struct _CustomData {
  GstElement *pipeline;               /* 主管道 */
  GstElement *videosink;              /* 视频输出元素 */

  GstElement *video_tee;              /* 视频 Tee 元素 */
  GstElement *audio_tee;              /* 音频 Tee 元素 */

  GtkWidget *sink_widget;             /* 视频显示组件 */
  GtkWidget *main_window;             /* 主窗口指针, 用于全屏/退出控制 */
  dictionary *config_dict;            /* 指向解析后的配置数据的指针 */

  gboolean has_tee;                   /* 标志是否存在 tee 元素 */
  gboolean is_recording;              /* 录制状态标志 */
  GtkWidget *record_icon;
} CustomData;

/*
 * 初始化并构建 GStreamer 管道
 * data: 指向 CustomData 结构体的指针
 * 返回值: 如果成功返回 TRUE，失败返回 FALSE
 */
gboolean initialize_gstreamer_pipeline(CustomData *data);

GstElement* create_and_add_element(const char *factory_name, const char *element_name, GstBin *bin);
void configure_element_from_ini(GstElement *element, dictionary *dict, const char *section_name);

#endif // CONFIG_H
