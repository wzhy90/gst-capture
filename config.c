#include "utils.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <iniparser.h>
#include <dictionary.h>

gboolean initialize_gstreamer_pipeline(CustomData *data) {
    dictionary *dict = data->config_dict;
    if (!dict) {
        g_printerr("Configuration data dictionary not available.\n");
        return FALSE;
    }

    data->pipeline = gst_pipeline_new("camera-pipeline");
    GstBin *bin = GST_BIN(data->pipeline);
    GstElement *last_video_element = NULL;
    GstElement *last_audio_element = NULL;
    gboolean success = TRUE;

    // 获取管道字符串 (使用 iniparser_getstring)
    const char *video_pipeline_str = iniparser_getstring(dict, "main:pipeline_video", NULL);

    // --- 1. 处理视频管道 ---
    if (video_pipeline_str) {
        // g_strsplit 使用 null 终止的字符串，iniparser_getstring 保证返回 null 终止的字符串
        gchar **elements_list = g_strsplit(video_pipeline_str, ",", -1);
        GstElement *prev_element = NULL;

        for (int i = 0; elements_list[i] != NULL; ++i) {
            char *ini_section_name = g_strstrip(elements_list[i]);
            if (strlen(ini_section_name) == 0) continue;

            GstElement *current_element = NULL;
            char element_gst_name[128];
            const char *factory_name = ini_section_name;
            const char *config_section_to_use = ini_section_name;

            // Video Tee
            if (strcmp(ini_section_name, "video_tee") == 0) {
                // 在这里插入 Tee 元素
                data->video_tee = create_and_add_element("tee", "video-tee", bin);
                if (!data->video_tee) { success = FALSE; break; }

                data->has_tee = TRUE;

                // 将前一个元素链接到 Tee
                if (prev_element && !gst_element_link(prev_element, data->video_tee)) {
                    g_printerr("Failed to link %s to video-tee.\n", GST_OBJECT_NAME(prev_element));
                    success = FALSE; break;
                } else {
#ifdef DEBUG
                    g_print("Linked %s to video-tee successfully.\n", GST_OBJECT_NAME(prev_element));
#endif
                }
                prev_element = data->video_tee; // Tee 成为下一个元素的 prev_element
                continue; // 跳过此迭代的剩余部分
            }

            if (strncmp(ini_section_name, "capsfilter", strlen("capsfilter")) == 0) {
                // 如果是 capsfilter 或 capsfilter1, factory name 统一用 capsfilter
                factory_name = "capsfilter";
            } else if (strncmp(ini_section_name, "vaapipostproc", strlen("vaapipostproc")) == 0) {
                 // 如果是 vaapipostproc 或 vaapipostproc1, factory name 统一用 vaapipostproc
                 factory_name = "vaapipostproc";
            } else if (strcmp(factory_name, "queue") == 0) {
                // queue 配置统一读取 [queue] 段，但 GStreamer 元素名称唯一
                config_section_to_use = "queue";
            }

            // 确保 GStreamer 内部名称唯一
            snprintf(element_gst_name, sizeof(element_gst_name), "%s-%d", factory_name, i);

            // 创建元素
            current_element = create_and_add_element(factory_name, element_gst_name, bin);

            if (current_element) {
                // 使用通用的配置函数，传入需要查找的 INI 段名
                configure_element_from_ini(current_element, dict, config_section_to_use);
            } else {
                success = FALSE;
                break;
            }

            if (prev_element) {
                if (!gst_element_link(prev_element, current_element)) {
                    g_printerr("Failed to link %s to %s.\n", GST_OBJECT_NAME(prev_element), GST_OBJECT_NAME(current_element));
                    success = FALSE;
                    break;
                } else {
#ifdef DEBUG
                    g_print("Linked %s to %s successfully.\n", GST_OBJECT_NAME(prev_element), GST_OBJECT_NAME(current_element));
#endif
                }
            }
            prev_element = current_element;
        }

        if (success) {
            last_video_element = prev_element;
        }
        g_strfreev(elements_list);
    } else {
        g_printerr("Missing 'main:pipeline_video' in INI file.\n");
        success = FALSE;
    }

    // --- 2. 添加并配置视频接收器 glsinkbin/gtkglsink ---
    if (success && last_video_element) {
        GstElement *gtkglsink = gst_element_factory_make("gtkglsink", "gtk-gl-sink");
        data->videosink = create_and_add_element("glsinkbin", "gl-sink-bin", bin);

        if (!gtkglsink || !data->videosink) {
            if (gtkglsink) gst_object_unref(gtkglsink);
            success = FALSE;
        } else {
            // 配置 glsinkbin 和 gtkglsink
            configure_element_from_ini(data->videosink, dict, "glsinkbin");
            configure_element_from_ini(gtkglsink, dict, "gtkglsink");
            g_object_set (data->videosink, "sink", gtkglsink, NULL); // 强制设置 sink 属性

            // 获取 gtkglsink 的 widget 用于 UI 显示
            g_object_get (gtkglsink, "widget", &data->sink_widget, NULL);

            if (!gst_element_link(last_video_element, data->videosink)) {
                g_printerr ("Failed to link %s to %s.\n", GST_OBJECT_NAME(last_video_element), GST_OBJECT_NAME(data->videosink));
                success = FALSE;
            } else {
#ifdef DEBUG
                g_print("Linked %s to %s successfully.\n", GST_OBJECT_NAME(last_video_element), GST_OBJECT_NAME(data->videosink));
#endif
            }
        }
    } else if (success) {
         g_printerr("Error: Video pipeline built successfully but last_video_element is NULL. Cannot add sink.\n");
         success = FALSE;
    }

    // --- 3. 处理音频管道 ---
    const char *audio_pipeline_str = iniparser_getstring(dict, "main:pipeline_audio", NULL);
    if (success && audio_pipeline_str) {
        gchar **elements_list = g_strsplit(audio_pipeline_str, ",", -1);
        GstElement *prev_element = NULL;
        char *last_audio_factory_name = NULL; 

        for (int i = 0; elements_list[i] != NULL; ++i) {
            char *ini_section_name = g_strstrip(elements_list[i]);
            if (strlen(ini_section_name) == 0) continue;

            if (elements_list[i+1] == NULL) {
                last_audio_factory_name = g_strdup(ini_section_name);
                break; // 退出循环，稍后手动创建sink
            }

            GstElement *current_element = NULL;
            char element_gst_name[128];
            const char *factory_name = ini_section_name;
            const char *config_section_to_use = ini_section_name;


            if (strncmp(ini_section_name, "capsfilter", strlen("capsfilter")) == 0) {
                // 如果是 capsfilter 或 capsfilter1, factory name 统一用 capsfilter
                factory_name = "capsfilter";
            } else if (strcmp(factory_name, "queue") == 0) {
                // 音频和视频队列共用 [queue] section
                config_section_to_use = "queue";
            }

            // 确保 GStreamer 内部名称唯一
            snprintf(element_gst_name, sizeof(element_gst_name), "%s-a%d", factory_name, i);

            // 创建元素
            current_element = create_and_add_element(factory_name, element_gst_name, bin);

            if (current_element) {
                // 使用通用的配置函数，传入需要查找的 INI 段名
                configure_element_from_ini(current_element, dict, config_section_to_use);
            } else {
                success = FALSE;
                break;
            }

            if (prev_element) {
                if (!gst_element_link(prev_element, current_element)) {
                    g_printerr("Failed to link %s to %s.\n", GST_OBJECT_NAME(prev_element), GST_OBJECT_NAME(current_element));
                    success = FALSE;
                    break;
                } else {
#ifdef DEBUG
                    g_print("Linked %s to %s successfully.\n", GST_OBJECT_NAME(prev_element), GST_OBJECT_NAME(current_element));
#endif
                }
            }
            prev_element = current_element;
        }

        // 循环结束后，prev_element 指向 pulsesink 之前的最后一个元素（可能是 queue）
        if (success && prev_element && last_audio_factory_name) {
            last_audio_element = prev_element;

            // --- 新增逻辑：创建 Audio Tee ---
            data->audio_tee = create_and_add_element("tee", "audio-tee", bin);
            if (!data->audio_tee) {
                success = FALSE;
            } else {
                // 将最后一个处理元素链接到 Tee
                if (!gst_element_link(last_audio_element, data->audio_tee)) {
                    g_printerr("Failed to link last audio element to audio-tee.\n");
                    success = FALSE;
                } else {
#ifdef DEBUG
                    g_print("Linked %s to %s successfully.\n", GST_OBJECT_NAME(last_audio_element), GST_OBJECT_NAME(data->audio_tee));
#endif
                }

                // --- 新增逻辑：创建 pulsesink 并链接到 Tee 的一个端口 (用于实时播放) ---
                GstElement *audio_sink = create_and_add_element(
                    last_audio_factory_name, 
                    "audio-sink",
                    bin
                );

                if (!audio_sink) success = FALSE;
                configure_element_from_ini(audio_sink, dict, last_audio_factory_name);

                if (success && !gst_element_link(data->audio_tee, audio_sink)) {
                     g_printerr("Failed to link audio-tee to audio-sink.\n");
                     success = FALSE;
                } else {
#ifdef DEBUG
                    g_print("Linked %s to %s successfully.\n", GST_OBJECT_NAME(data->audio_tee), GST_OBJECT_NAME(audio_sink));
#endif
                }
            }
        } else if (success && !last_audio_factory_name) {
            g_printerr("Error: Could not determine last audio sink element name from INI config.\n");
            success = FALSE;
        }

        if (success) {
            last_audio_element = prev_element;
        }
        g_strfreev(elements_list);
        if (last_audio_factory_name) {
            g_free(last_audio_factory_name);
        }
    }

    if (!success) {
        g_printerr("Pipeline initialization failed. Cleaning up.\n");
        if (data->pipeline) {
            gst_object_unref(data->pipeline);
            data->pipeline = NULL;
        }
        return FALSE;
    }

    return TRUE;
}

