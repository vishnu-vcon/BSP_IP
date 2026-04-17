/*
 * recording.h — Recording Branch
 * ================================
 */

#ifndef SMARTIP_RECORDING_H
#define SMARTIP_RECORDING_H

#include <gst/gst.h>
#include "subsystems/sys_capture.h"

typedef struct _UnifiedEngine UnifiedEngine;

typedef struct {
    char type[16];   /* "none", "once", "daily" */
    
    /* "once" fields */
    time_t real_start;
    time_t real_end;
    
    /* "daily" fields (Seconds since midnight) */
    int daily_start_sec;
    int daily_end_sec;
} ScheduleConfig;

typedef struct _RecordingBranch {
    UnifiedEngine   *engine;
    CapturePipeline *capture;
    char             lens[32];
    char             tier[32];
    char             output_dir[256];
    char             mode[16];       /* "continuous" or "event" */
    int              idle_timeout;
    int              max_segment_sec;
    gboolean         running;
    gboolean         armed;
    double           deadline;
    guint            event_timeout_id;  /* Dedicated watchdog for Layered AI Trigger */
    char             branch_name[64];
    /* Config Snapshot (Memory Safety: de-coupled from master config lifecycle) */
    char             codec[16];
    char             resolution[16];
    int              width, height, fps, bitrate;
    gboolean         ntp_overlay;
    ScheduleConfig   sched;
    GMutex           rec_lock;       /* Protects running/armed/deadline state */
    gboolean         finished;       /* True if a "once" schedule is totally done */
} RecordingBranch;

RecordingBranch *recording_branch_new  (UnifiedEngine *engine, CapturePipeline *capture_pipe,
                                         const char *lens, const char *tier,
                                         const char *output_dir, BranchConfig *recording_cfg,
                                         const char *mode, int idle_timeout, int max_segment_sec);
gboolean         recording_branch_start(RecordingBranch *recording_branch);
void             recording_branch_stop (RecordingBranch *recording_branch);
void             recording_branch_poke (RecordingBranch *recording_branch);
void             recording_branch_free (RecordingBranch *recording_branch);

/* ── Scheduler Utilities ── */
gboolean         recording_is_overlapping(RecordingBranch *branch_a, RecordingBranch *branch_b);
gpointer         recording_scheduler_loop(gpointer data);
void             recording_scheduler_stop(UnifiedEngine *engine);
void             engine_persist_state(UnifiedEngine *engine);

#endif /* SMARTIP_RECORDING_H */
