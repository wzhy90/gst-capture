#ifndef UTILS_H
#define UTILS_H

#include <gst/gst.h>
#include <iniparser/iniparser.h>
#include <iniparser/dictionary.h>

/*
 * Helper function: Dynamically create an element and add it to a bin
 */
GstElement* create_and_add_element(const char *factory_name, const char *element_name, GstBin *bin);

/*
 * Helper function: Manually convert string values to GStreamer property types and set the property
 */
void set_element_property(GstElement *element, const char *key_name, const char *value_str);

/*
 * Helper function: Configure an element using settings from an INI dictionary
 */
void configure_element_from_ini(GstElement *element, dictionary *dict, const char *section_name);

#endif // UTILS_H

