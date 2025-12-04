#include "utils.h"
#include <glib-object.h>
#include <gst/gst.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <iniparser/iniparser.h>
#include <iniparser/dictionary.h>

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

static gboolean parse_flags_string(GType flags_type, const gchar *value_str, gint *out_value) {
    g_autoptr(GFlagsClass) flags_class = g_type_class_ref(flags_type);
    g_autofree gchar **flags_nicks = NULL;
    gint combined_value = 0;
    gboolean success = TRUE;

    flags_nicks = g_strsplit_set(value_str, "|,", -1);

    for (int i = 0; flags_nicks != NULL && flags_nicks[i] != NULL; i++) {
        const gchar *nick = g_strstrip(flags_nicks[i]);

        if (strlen(nick) == 0) continue;

        GFlagsValue *flag_value = g_flags_get_value_by_nick(flags_class, nick);
        if (flag_value == NULL) {
            flag_value = g_flags_get_value_by_name(flags_class, nick);
        }

        if (flag_value != NULL) {
            combined_value |= flag_value->value;
        } else {
            g_printerr("Error: Unknown flag '%s' for type %s.\n", nick, g_type_name(flags_type));
            success = FALSE;
            break;
        }
    }

    if (success) {
        *out_value = combined_value;
    }

    return success;
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
    const gchar *type_name = g_type_name(value_type);
    gboolean success = FALSE;
    gint int_val = 0;

    if (G_TYPE_IS_ENUM(value_type)) {
        GEnumValue *enum_value;
        g_autoptr(GEnumClass) enum_class = g_type_class_ref(value_type);

        enum_value = g_enum_get_value_by_nick(enum_class, value_str);
        if (enum_value == NULL) {
             enum_value = g_enum_get_value_by_name(enum_class, value_str);
        }

        if (enum_value != NULL) {
            int_val = enum_value->value;
            success = TRUE;
        } else {
            char *endptr;
            long num_val = strtol(value_str, &endptr, 10);
            if (*endptr == '\0' && value_str[0] != '\0') {
               int_val = (gint)num_val;
               success = TRUE;
            }
        }
    } else if (G_TYPE_IS_FLAGS(value_type)) {
        success = parse_flags_string(value_type, value_str, &int_val);
    }

    if (success) {
        g_object_set(G_OBJECT(element), key_name, int_val, NULL);
    } else if (g_strcmp0(type_name, "GstCaps") == 0) {
        // 直接匹配 "GstCaps" 类型名称，并进行特殊处理
        g_autoptr(GstCaps) caps = gst_caps_from_string(value_str);
        if (caps) {
            g_object_set(G_OBJECT(element), key_name, caps, NULL);
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
        gint64 long_val = (gint64)strtoll(value_str, NULL, 10);
        g_object_set(G_OBJECT(element), key_name, long_val, NULL);
        success = TRUE;
    } else if (g_strcmp0(type_name, "gfloat") == 0 || g_strcmp0(type_name, "gdouble") == 0) {
        gdouble double_val = g_strtod(value_str, NULL);
        g_object_set(G_OBJECT(element), key_name, double_val, NULL);
        success = TRUE;
    }

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
#ifdef DEBUG
        g_print("Section [%s] exists but contains no keys to configure.\n", section_ptr);
#endif
        return;
    }

    // 分配内存来存储指向键字符串的指针数组
    const char **keys = g_newa(const char*, num_keys);

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

