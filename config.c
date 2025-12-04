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

    const char *video_pipeline_str = iniparser_getstring(dict, "main:pipeline_video", NULL);

    // --- 1. 处理视频管道 ---
    if (video_pipeline_str) {
        g_autofree gchar **elements_list = g_strsplit(video_pipeline_str, ",", -1);
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
                data->video_tee = create_and_add_element("tee", "video-tee", bin);
                if (!data->video_tee) { success = FALSE; break; }

                data->has_tee = TRUE;

                if (prev_element && !gst_element_link(prev_element, data->video_tee)) {
                    g_printerr("Failed to link %s to video-tee.\n", GST_OBJECT_NAME(prev_element));
                    success = FALSE; break;
                } else {
#ifdef DEBUG
                    g_print("Linked %s to video-tee successfully.\n", GST_OBJECT_NAME(prev_element));
#endif
                }
                prev_element = data->video_tee;
                continue;
            }

            if (strncmp(ini_section_name, "capsfilter", strlen("capsfilter")) == 0) {
                factory_name = "capsfilter";
            } else if (strncmp(ini_section_name, "vaapipostproc", strlen("vaapipostproc")) == 0) {
                 factory_name = "vaapipostproc";
            } else if (strcmp(factory_name, "queue") == 0) {
                config_section_to_use = "queue";
            }

            snprintf(element_gst_name, sizeof(element_gst_name), "%s-%d", factory_name, i);

            current_element = create_and_add_element(factory_name, element_gst_name, bin);

            if (current_element) {
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
            configure_element_from_ini(data->videosink, dict, "glsinkbin");
            configure_element_from_ini(gtkglsink, dict, "gtkglsink");
            g_object_set (data->videosink, "sink", gtkglsink, NULL);

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
        g_autofree gchar **elements_list = g_strsplit(audio_pipeline_str, ",", -1);
        GstElement *prev_element = NULL;
        g_autofree char *last_audio_factory_name = NULL; 

        for (int i = 0; elements_list[i] != NULL; ++i) {
            char *ini_section_name = g_strstrip(elements_list[i]);
            if (strlen(ini_section_name) == 0) continue;

            if (elements_list[i+1] == NULL) {
                last_audio_factory_name = g_strdup(ini_section_name);
                break;
            }

            GstElement *current_element = NULL;
            char element_gst_name[128];
            const char *factory_name = ini_section_name;
            const char *config_section_to_use = ini_section_name;


            if (strncmp(ini_section_name, "capsfilter", strlen("capsfilter")) == 0) {
                factory_name = "capsfilter";
            } else if (strcmp(factory_name, "queue") == 0) {
                config_section_to_use = "queue";
            }

            snprintf(element_gst_name, sizeof(element_gst_name), "%s-a%d", factory_name, i);

            current_element = create_and_add_element(factory_name, element_gst_name, bin);

            if (current_element) {
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

        if (success && prev_element && last_audio_factory_name) {
            last_audio_element = prev_element;

            data->audio_tee = create_and_add_element("tee", "audio-tee", bin);
            if (!data->audio_tee) {
                success = FALSE;
            } else {
                if (!gst_element_link(last_audio_element, data->audio_tee)) {
                    g_printerr("Failed to link last audio element to audio-tee.\n");
                    success = FALSE;
                } else {
#ifdef DEBUG
                    g_print("Linked %s to %s successfully.\n", GST_OBJECT_NAME(last_audio_element), GST_OBJECT_NAME(data->audio_tee));
#endif
                }

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

