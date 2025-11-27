#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <iniparser.h>
#include <dictionary.h>

// 辅助函数：动态创建元素并添加到bin
GstElement* create_and_add_element(const char *factory_name, const char *element_name, GstBin *bin) {
    GstElement *element = gst_element_factory_make(factory_name, element_name);
    if (!element) {
        g_printerr("Failed to create element of type %s with name %s.\n", factory_name, element_name);
        return NULL;
    }
    gst_bin_add(bin, element);
#ifdef DEBUG
    g_print("Created element: %s (%s) and added to pipeline.\n", element_name, factory_name);
#endif
    return element;
}

// 辅助函数：手动将字符串值转换为 GStreamer 属性类型并设置
void set_element_property(GstElement *element, const char *key_name, const char *value_str) {
    GParamSpec *pspec;
    pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), key_name);

    if (!pspec) {
#ifdef DEBUG
        g_print("  Property '%s' not found on element %s. Skipping.\n", key_name, GST_OBJECT_NAME(element));
#endif
        return;
    }

    GType value_type = G_PARAM_SPEC_VALUE_TYPE(pspec);
    const gchar *type_name = g_type_name(value_type); // 获取类型名称字符串

    gboolean success = FALSE;

    // 根据类型名称字符串进行匹配和手动转换
    if (g_strcmp0(type_name, "GstCaps") == 0) {
        // 直接匹配 "GstCaps" 类型名称，并进行特殊处理
        GstCaps *caps = gst_caps_from_string(value_str);
        if (caps) {
            g_object_set(G_OBJECT(element), key_name, caps, NULL);
            gst_caps_unref(caps);
            success = TRUE;
        }
    } else if (g_strcmp0(type_name, "gchararray") == 0) {
        // 普通字符串属性
        g_object_set(G_OBJECT(element), key_name, value_str, NULL);
        success = TRUE;
    } else if (g_strcmp0(type_name, "gint") == 0 || g_strcmp0(type_name, "guint") == 0) {
        // 使用标准 C 库的 strtol
        gint int_val = (gint)strtol(value_str, NULL, 10);
        g_object_set(G_OBJECT(element), key_name, int_val, NULL);
        success = TRUE;
    } else if (g_strcmp0(type_name, "gboolean") == 0) {
        gboolean bool_val = (gboolean)(iniparser_getboolean(NULL, value_str, 0) == 1);
        g_object_set(G_OBJECT(element), key_name, bool_val, NULL);
        success = TRUE;
    } else if (g_strcmp0(type_name, "gint64") == 0 || g_strcmp0(type_name, "guint64") == 0 ||
               g_strcmp0(type_name, "glong") == 0 || g_strcmp0(type_name, "gulong") == 0) {
        // 使用标准 C 库的 strtoll
        gint64 long_val = (gint64)strtoll(value_str, NULL, 10);
        g_object_set(G_OBJECT(element), key_name, long_val, NULL);
        success = TRUE;
    } else if (g_strcmp0(type_name, "gfloat") == 0 || g_strcmp0(type_name, "gdouble") == 0) {
        // 使用 GLib 的 g_strtod
        gdouble double_val = g_strtod(value_str, NULL);
        g_object_set(G_OBJECT(element), key_name, double_val, NULL);
        success = TRUE;
    } else if (g_strcmp0(type_name, "GstVideoFormat") == 0) {
        gint int_val = (gint)strtol(value_str, NULL, 10);
        g_object_set(G_OBJECT(element), key_name, int_val, NULL);
        success = TRUE;
    }
    // 注意：枚举类型 (GstEnum) 和标志类型 (GstFlags) 需要更复杂的处理，这里暂不支持。

    if (success) {
#ifdef DEBUG
        g_print("  Property '%s' (Type: %s) set to '%s'.\n", key_name, type_name, value_str);
#endif
    } else {
        g_printerr("Warning: Unsupported property type (%s) or failed conversion for key '%s' on element %s. Value '%s' ignored.\n",
                   type_name, key_name, GST_OBJECT_NAME(element), value_str);
    }
}

// 辅助函数：根据INI配置设置元素属性
void configure_element_from_ini(GstElement *element, dictionary *dict, const char *section_name) {
    if (!element || !dict || !section_name) return;

    // iniparser section names don't include brackets []
    const char *section_ptr = section_name;

#ifdef DEBUG
    g_print("Configuring element [%s] from INI section [%s]:\n", GST_OBJECT_NAME(element), section_name);
#endif

    // 获取该 section 的键数量
    int num_keys = iniparser_getsecnkeys(dict, section_ptr);
    if (num_keys == 0) {
        // g_print("Section [%s] exists but contains no keys to configure.\n", section_ptr);
        return;
    }

    // 分配内存来存储指向键字符串的指针数组
    const char **keys = g_newa(const char*, num_keys);

    // 使用正确的函数 iniparser_getseckeys 来填充 keys 数组
    if (!iniparser_getseckeys(dict, section_ptr, keys)) {
        g_printerr("Failed to retrieve keys for section [%s].\n", section_ptr);
        return;
    }

    for (int i = 0; i < num_keys; ++i) {
        // full_key 现在是 "section:key" 格式的字符串指针
        const char *full_key = keys[i];
        if (!full_key) continue;

        // 提取 key 名称 (跳过 "section:")
        const char *key_name = strchr(full_key, ':');
        if (!key_name) continue;
        key_name++; // 指向实际的属性名

        // 使用 full_key 获取值
        const char *value_str = iniparser_getstring(dict, full_key, NULL);
        if (!value_str) continue;

        // 使用辅助函数来设置属性
        set_element_property(element, key_name, value_str);
    }
}

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

