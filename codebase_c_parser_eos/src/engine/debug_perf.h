/* ============================================================================
 * debug_perf.h  —  Unified Performance & Debug Instrumentation Header
 *
 * NOTE: PERF DEBUGGING IS NOW PERMANENTLY DISABLED IN THIS BUILD.
 * ============================================================================ */

#ifndef SMARTIP_DEBUG_PERF_H
#define SMARTIP_DEBUG_PERF_H

#include <glib.h>

/* ── Always disabled flag ── */
static inline gboolean _perf_debug_enabled(void) {
    return FALSE;
}

/* ── Monotonic clock helper (nanoseconds) - returns 0 in non-perf builds ── */
static inline gint64 _perf_now_ns(void) {
    return 0;
}

/* ── Function-level profiling (Disabled) ── */
#define PERF_FUNC_START() 
#define PERF_FUNC_END(label) 

/* ── Section-level (sub-function) profiling (Disabled) ── */
#define PERF_SECTION_START(var) 
#define PERF_SECTION_END(var, label) 

/* ── Buffer/sample flow counters (thread-safe) (Disabled) ── */
typedef struct {
    const char *name;
    volatile gint  count;
    volatile gint  drop_count;
    gint64         last_log_ns;
} PerfCounter;

#define PERF_COUNTER_DEF(varname, countername) \
    static PerfCounter varname = { (countername), 0, 0, 0 }

#define PERF_COUNTER_INC(ctr) 
#define PERF_COUNTER_DROP(ctr) 
#define PERF_COUNTER_LOG_EVERY(ctr, n) 

/* ── Lock contention tracing (Standard Locks) ── */
#define PERF_LOCK(mutex, label) g_mutex_lock(&(mutex))
#define PERF_UNLOCK(mutex) g_mutex_unlock(&(mutex))

/* ── Tracing Macros (Disabled) ── */
#define OVERLAY_TRACE(fmt, ...) 
#define ELEM_TRACE(name, factory, result) 
#define STATE_TRACE(element, from_state, to_state) 
#define PROP_TRACE(element, ...) 

#endif /* SMARTIP_DEBUG_PERF_H */
