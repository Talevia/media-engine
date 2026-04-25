/*
 * me_jni_progress — internal helpers extracted from me_jni.cpp
 * to keep that TU under §1a's 400-line ceiling. me_jni.cpp was
 * at 335 lines + still growing per cycle (cycle 76 thumbnail
 * +30, cycle 84 last_status +20, cycle 92 render_frame +50);
 * splitting now is preemptive.
 *
 * Three pieces live here:
 *   - g_jni_last_status: thread-local me_status_t captured by
 *     each native* call's failure path. Read by
 *     nativeLastStatus to surface ME_E_* fidelity to the
 *     Java side. Declared `extern thread_local` so callers
 *     (the JNI exports in me_jni.cpp) can write to it.
 *   - JavaCallback / JobWrap: per-renderjob bookkeeping
 *     for the progress-listener bridge.
 *   - progress_trampoline: the C-callback bridge that the
 *     engine's worker thread invokes; attaches the JVM,
 *     calls the Java listener, swallows exceptions, detaches.
 *
 * This header is private (under bindings/jni/), never shipped.
 * me_jni.cpp #includes it; no third-party consumers.
 */
#pragma once

#include <jni.h>
#include <media_engine.h>

namespace me::jni::detail {

/* See cpp for rationale. Defined in me_jni_progress.cpp. */
extern thread_local me_status_t g_jni_last_status;

struct JavaCallback {
    JavaVM*   jvm         = nullptr;
    jobject   listener    = nullptr;   /* global ref */
    jmethodID on_progress = nullptr;
};

struct JobWrap {
    me_render_job_t* job = nullptr;
    JavaCallback*    cb  = nullptr;    /* nullable — no listener passed */
};

/* Engine-side me_progress_cb that bridges to the Java listener
 * via cb->on_progress(kind, ratio, message). Re-entrant safe;
 * attaches the calling thread to the JVM if it's detached and
 * detaches before returning. */
void progress_trampoline(const me_progress_event_t* ev, void* user);

}  // namespace me::jni::detail
