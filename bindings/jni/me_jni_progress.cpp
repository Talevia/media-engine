/*
 * Implementation of the helpers declared in me_jni_progress.hpp.
 * See header for the extraction rationale.
 */
#include "me_jni_progress.hpp"

namespace me::jni::detail {

/* Per-thread last me_status_t captured when a native* call returns
 * a failure status. Without this, the Java side only sees the "0
 * = failure" sentinel from each long-returning native; status code
 * fidelity is lost (ME_E_PARSE vs ME_E_INVALID_ARG vs ME_E_IO etc.
 * all collapse to "RuntimeException: <error string>"). Hosts that
 * want graceful retry-vs-bail logic need the structured code.
 *
 * Thread-local because native* calls can interleave across threads
 * in JVM hosts; one engine handle may be touched from multiple JVM
 * threads. Keeps the last-status discipline analogous to
 * me_engine_last_error's thread-local storage. */
thread_local me_status_t g_jni_last_status = ME_OK;

/* Invoked by the engine's worker thread. Must be re-entrant safe and
 * must not leak JNI local refs (worker threads are short-lived but
 * the LocalRefTable isn't auto-popped like normal native calls). */
void progress_trampoline(const me_progress_event_t* ev, void* user) {
    auto* cb = static_cast<JavaCallback*>(user);
    if (!cb || !cb->jvm || !cb->listener || !cb->on_progress) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    jint getEnvRc = cb->jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8);
    if (getEnvRc == JNI_EDETACHED) {
        if (cb->jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) {
            return;
        }
        attached = true;
    } else if (getEnvRc != JNI_OK) {
        return;
    }

    jstring msg = ev->message ? env->NewStringUTF(ev->message) : nullptr;
    env->CallVoidMethod(cb->listener, cb->on_progress,
                        static_cast<jint>(ev->kind),
                        static_cast<jfloat>(ev->ratio),
                        msg);
    if (env->ExceptionCheck()) {
        /* Callback implementations shouldn't throw, but if they do,
         * swallow to keep the engine alive; the Java side can still
         * read last-error after waitFor. */
        env->ExceptionClear();
    }
    if (msg) env->DeleteLocalRef(msg);

    if (attached) cb->jvm->DetachCurrentThread();
}

}  // namespace me::jni::detail
