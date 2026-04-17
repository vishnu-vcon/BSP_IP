/*
 * hls_generator.h — HLS Stream Generator
 * ======================================
 * Consumes encoded buffers from StreamManager and writes HLS segments.
 */

#ifndef SMARTIP_HLS_GENERATOR_H
#define SMARTIP_HLS_GENERATOR_H

#include <glib.h>
#include <gst/gst.h>
#include "subsystems/sys_stream.h"

typedef struct _HLSGenerator HLSGenerator;

HLSGenerator *hls_generator_new(StreamManager *stream_mgr, 
                               const char *lens_name, 
                               const char *tier_name,
                               const char *codec_name,
                               const char *output_dir);

void          hls_generator_free(HLSGenerator *hls_gen);

void          hls_generator_start(HLSGenerator *hls_gen);
void          hls_generator_stop(HLSGenerator *hls_gen);

void          hls_generator_refresh(HLSGenerator *hls_gen);
gint64        hls_generator_get_idle_time_sec(HLSGenerator *hls_gen);
GstElement*   hls_generator_get_pipeline(HLSGenerator *hls_gen);

#endif /* SMARTIP_HLS_GENERATOR_H */
