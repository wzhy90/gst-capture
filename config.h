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

  GstElement *recording_bin;          /* 录制子管道容器 (GstBin) */
  GstPad *video_tee_q_pad;            /* 从视频 Tee 请求的 Pad (用于取消链接和释放) */
  GstPad *audio_tee_q_pad;            /* 从音频 Tee 请求的 Pad (用于取消链接和释放) */

  GtkWidget *sink_widget;             /* 视频显示组件 */
  GtkWidget *main_window;             /* 主窗口指针, 用于全屏/退出控制 */
  dictionary *config_dict;            /* 指向解析后的配置数据的指针 */

  gboolean has_tee;                   /* 标志是否存在 tee 元素 */
  gboolean is_recording;              /* 录制状态标志 */
  gboolean is_stopping_recording;     /* 正在停止/清理过程中的标志 */
  gchar *recording_filename;          /* 录制文件名指针 */
  GtkWidget *record_icon;             /* 录制图标指针 */
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
