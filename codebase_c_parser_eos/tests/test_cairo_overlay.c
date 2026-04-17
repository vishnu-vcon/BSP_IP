#include <cairo.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>

/* Structure to hold our data */
typedef struct _CustomData {
  GstElement *pipeline;
  GstElement *source;
  GstElement *cairo_overlay;
  GstElement *sink;

  /* Mock inference data: [x, y, width, height] */
  double bx, by, bw, bh;
  char *label;
} CustomData;

/* Cairo drawing callback */
static void on_draw(GstElement *overlay, cairo_t *cr, guint64 timestamp,
                    guint64 duration, CustomData *data) {
  (void)overlay;
  (void)timestamp;
  (void)duration;

  /* Get the actual dimensions of the video frame */
  double x1, y1, x2, y2;
  cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
  double width = x2 - x1;
  double height = y2 - y1;

  /* Draw a large, centered bounding box (normalized: 20% to 80% of frame) */
  double bx = width * 0.2;
  double by = height * 0.2;
  double bw = width * 0.6;
  double bh = height * 0.6;

  /* Set drawing parameters */
  cairo_set_line_width(cr, width * 0.005); /* Scalable line thickness */
  cairo_set_source_rgb(cr, 1.0, 0.0, 0.0); /* Red bounding box */

  /* Draw bounding box */
  cairo_rectangle(cr, bx, by, bw, bh);
  cairo_stroke(cr);

  /* Draw label background */
  cairo_set_source_rgb(cr, 1.0, 0.0, 0.0);
  double label_h = height * 0.04;
  cairo_rectangle(cr, bx, by - label_h, width * 0.15, label_h);
  cairo_fill(cr);

  /* Draw label text */
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0); /* White text */
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, label_h * 0.8);
  cairo_move_to(cr, bx + 5, by - (label_h * 0.2));
  cairo_show_text(cr, data->label);

  /* Output dimensions for verification in logs */
  static int once = 0;
  if (!once) {
      g_print("Detected frame resolution: %.0f x %.0f\n", width, height);
      once = 1;
  }
}

int main(int argc, char *argv[]) {
  CustomData data;
  GstBus *bus;
  GstMessage *msg;
  GstStateChangeReturn ret;

  /* Initialize GStreamer */
  gst_init(&argc, &argv);

  /* Initialize mock data */
  data.bx = 100;
  data.by = 100;
  data.bw = 200;
  data.bh = 150;
  data.label = "Person: 0.98";

  /* Create the elements */
  /* Using v4l2src for camera, fallback to videotestsrc if needed */
  data.source = gst_element_factory_make("v4l2src", "source");
  if (!data.source) {
    g_print("v4l2src not found, using videotestsrc\n");
    data.source = gst_element_factory_make("videotestsrc", "source");
    g_object_set(data.source, "is-live", TRUE, NULL);
  } else {
    /* Set camera device - common on i.MX8MP */
    g_object_set(data.source, "device", "/dev/video3", NULL);
  }

  /* Use i.MX specific converter if available for better performance */
  const char *conv_name = gst_element_factory_find("imxvideoconvert_g2d")
                              ? "imxvideoconvert_g2d"
                              : "videoconvert";
  GstElement *vconv1 = gst_element_factory_make(conv_name, "vconv1");
  data.cairo_overlay = gst_element_factory_make("cairooverlay", "overlay");
  GstElement *vconv2 = gst_element_factory_make(conv_name, "vconv2");

  /* Try waylandsink, fallback to autovideosink */
  data.sink = gst_element_factory_make("waylandsink", "sink");
  if (!data.sink) {
    data.sink = gst_element_factory_make("autovideosink", "sink");
  }

  /* Create the empty pipeline */
  data.pipeline = gst_pipeline_new("cairo-test-pipeline");

  if (!data.pipeline || !data.source || !vconv1 || !data.cairo_overlay ||
      !vconv2 || !data.sink) {
    g_printerr("Not all elements could be created.\n");
    return -1;
  }

  /* Build the pipeline */
  gst_bin_add_many(GST_BIN(data.pipeline), data.source, vconv1,
                   data.cairo_overlay, vconv2, data.sink, NULL);
  if (gst_element_link_many(data.source, vconv1, data.cairo_overlay, vconv2,
                            data.sink, NULL) != TRUE) {
    g_printerr("Elements could not be linked.\n");
    gst_object_unref(data.pipeline);
    return -1;
  }

  /* Connect to the draw signal */
  g_signal_connect(data.cairo_overlay, "draw", G_CALLBACK(on_draw), &data);

  /* Start playing */
  ret = gst_element_set_state(data.pipeline, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    g_printerr("Unable to set the pipeline to the playing state.\n");
    gst_object_unref(data.pipeline);
    return -1;
  }

  /* Wait until error or EOS */
  bus = gst_element_get_bus(data.pipeline);
  msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                   GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  /* Parse message */
  if (msg != NULL) {
    GError *err;
    gchar *debug_info;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR:
      gst_message_parse_error(msg, &err, &debug_info);
      g_printerr("Error received from element %s: %s\n",
                 GST_OBJECT_NAME(msg->src), err->message);
      g_printerr("Debugging information: %s\n",
                 debug_info ? debug_info : "none");
      g_error_free(err);
      g_free(debug_info);
      break;
    case GST_MESSAGE_EOS:
      g_print("End-Of-Stream reached.\n");
      break;
    default:
      /* For anything else, just print the message type */
      g_printerr("Unexpected message received.\n");
      break;
    }
    gst_message_unref(msg);
  }

  /* Free resources */
  gst_object_unref(bus);
  gst_element_set_state(data.pipeline, GST_STATE_NULL);
  gst_object_unref(data.pipeline);
  return 0;
}
