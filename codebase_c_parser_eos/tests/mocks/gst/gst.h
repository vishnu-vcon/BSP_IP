#ifndef MOCK_GST_H
#define MOCK_GST_H

#include <glib.h>
#include <glib-object.h>

typedef struct _GstElement GstElement;
typedef struct _GstElementFactory GstElementFactory;
typedef struct _GstCaps GstCaps;
typedef struct _GstStructure GstStructure;
typedef struct _GstPluginFeature GstPluginFeature;

typedef enum {
    GST_PLUGIN_FEATURE_TYPE_ELEMENT = 1
} GstPluginFeatureType;

#define GST_PLUGIN_FEATURE(obj) ((GstPluginFeature*)(obj))

GstElement *gst_element_factory_make(const gchar *factoryname, const gchar *name);
GstElementFactory *gst_element_factory_find(const gchar *name);
GstCaps *gst_caps_from_string(const gchar *string);
void gst_caps_unref(GstCaps *caps);
GstElementFactory *gst_element_get_factory(GstElement *element);
const gchar *gst_plugin_feature_get_name(GstPluginFeature *feature);
GstStructure *gst_structure_new(const gchar *name, const gchar *first_field, ...);
void gst_structure_free(GstStructure *structure);

#endif
