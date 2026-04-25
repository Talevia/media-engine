/*
 * JNI glue for io.mediaengine.MediaEngine.
 *
 * Translates each `native` method on the Java side into a call against
 * media-engine's C API. Ownership model:
 *
 *   engine handle  : allocated in nativeCreate, freed in nativeDestroy.
 *   timeline handle: allocated in nativeLoadTimeline, freed in
 *                    nativeTimelineDestroy.
 *   render-job     : represented on the Java side as one `long` but
 *                    wraps a `JobWrap` struct carrying (me_render_job_t*,
 *                    JavaCallback*); the listener's global ref lives
 *                    inside JavaCallback and is released by
 *                    nativeRenderJobDestroy.
 *
 * Progress callbacks run on an engine-owned worker thread — we
 * attach/detach per event (simple; not hot-path). A JavaVM* handle is
 * captured on the caller's thread during nativeRenderStart.
 *
 * Error surfacing: the C API writes detailed error text into a
 * thread-local slot readable via me_engine_last_error. The Java
 * wrapper throws RuntimeException at the native-call boundary when a
 * `long` out-param comes back zero and attaches that message. This
 * file therefore does NOT throw Java exceptions from inside native
 * methods; it simply returns 0 / default.
 */
#include <jni.h>
#include <media_engine.h>

#include "me_jni_progress.hpp"

#include <cstddef>
#include <cstdint>

/* Pull the helpers extracted to me_jni_progress.{hpp,cpp} into
 * file-local view — keeps the JNIEXPORT bodies below readable
 * without a `me::jni::detail::` qualifier on every reference. */
using me::jni::detail::g_jni_last_status;
using me::jni::detail::JavaCallback;
using me::jni::detail::JobWrap;
using me::jni::detail::progress_trampoline;

extern "C" {

JNIEXPORT jlong JNICALL
Java_io_mediaengine_MediaEngine_nativeCreate(JNIEnv*, jclass) {
    me_engine_t* eng = nullptr;
    me_status_t s = me_engine_create(nullptr, &eng);
    g_jni_last_status = s;
    if (s != ME_OK) return 0;
    return reinterpret_cast<jlong>(eng);
}

JNIEXPORT void JNICALL
Java_io_mediaengine_MediaEngine_nativeDestroy(JNIEnv*, jclass, jlong h) {
    me_engine_destroy(reinterpret_cast<me_engine_t*>(h));
}

JNIEXPORT jlong JNICALL
Java_io_mediaengine_MediaEngine_nativeLoadTimeline(JNIEnv* env, jclass,
                                                    jlong eng_h, jstring json) {
    auto* eng = reinterpret_cast<me_engine_t*>(eng_h);
    const char* json_c = env->GetStringUTFChars(json, nullptr);
    const jsize json_len = env->GetStringUTFLength(json);
    me_timeline_t* tl = nullptr;
    me_status_t s = me_timeline_load_json(eng, json_c,
                                           static_cast<std::size_t>(json_len), &tl);
    env->ReleaseStringUTFChars(json, json_c);
    g_jni_last_status = s;
    if (s != ME_OK) return 0;
    return reinterpret_cast<jlong>(tl);
}

JNIEXPORT void JNICALL
Java_io_mediaengine_MediaEngine_nativeTimelineDestroy(JNIEnv*, jclass, jlong h) {
    me_timeline_destroy(reinterpret_cast<me_timeline_t*>(h));
}

JNIEXPORT jlong JNICALL
Java_io_mediaengine_MediaEngine_nativeRenderStart(
    JNIEnv* env, jclass,
    jlong eng_h, jlong tl_h,
    jstring path, jstring container, jstring video_codec, jstring audio_codec,
    jlong video_bitrate, jlong audio_bitrate,
    jint width, jint height,
    jlong fr_num, jlong fr_den,
    jobject listener) {

    const char* path_c  = env->GetStringUTFChars(path, nullptr);
    const char* cont_c  = container   ? env->GetStringUTFChars(container, nullptr)   : nullptr;
    const char* vcod_c  = video_codec ? env->GetStringUTFChars(video_codec, nullptr) : nullptr;
    const char* acod_c  = audio_codec ? env->GetStringUTFChars(audio_codec, nullptr) : nullptr;

    me_output_spec_t spec{};
    spec.path              = path_c;
    spec.container         = cont_c;
    spec.video_codec       = vcod_c;
    spec.audio_codec       = acod_c;
    spec.video_bitrate_bps = static_cast<int64_t>(video_bitrate);
    spec.audio_bitrate_bps = static_cast<int64_t>(audio_bitrate);
    spec.width             = static_cast<int>(width);
    spec.height            = static_cast<int>(height);
    spec.frame_rate        = me_rational_t{static_cast<int64_t>(fr_num),
                                           static_cast<int64_t>(fr_den)};

    JavaCallback* cb = nullptr;
    me_progress_cb trampoline = nullptr;
    if (listener) {
        cb = new JavaCallback();
        env->GetJavaVM(&cb->jvm);
        cb->listener = env->NewGlobalRef(listener);
        jclass lcl = env->GetObjectClass(listener);
        cb->on_progress = env->GetMethodID(lcl, "onProgress", "(IFLjava/lang/String;)V");
        env->DeleteLocalRef(lcl);
        if (!cb->on_progress) {
            env->DeleteGlobalRef(cb->listener);
            delete cb;
            cb = nullptr;
        } else {
            trampoline = progress_trampoline;
        }
    }

    me_render_job_t* job = nullptr;
    me_status_t s = me_render_start(reinterpret_cast<me_engine_t*>(eng_h),
                                     reinterpret_cast<me_timeline_t*>(tl_h),
                                     &spec, trampoline, cb, &job);

    env->ReleaseStringUTFChars(path, path_c);
    if (cont_c) env->ReleaseStringUTFChars(container,   cont_c);
    if (vcod_c) env->ReleaseStringUTFChars(video_codec, vcod_c);
    if (acod_c) env->ReleaseStringUTFChars(audio_codec, acod_c);

    g_jni_last_status = s;
    if (s != ME_OK) {
        if (cb) {
            env->DeleteGlobalRef(cb->listener);
            delete cb;
        }
        return 0;
    }

    auto* w = new JobWrap{job, cb};
    return reinterpret_cast<jlong>(w);
}

JNIEXPORT jint JNICALL
Java_io_mediaengine_MediaEngine_nativeRenderWait(JNIEnv*, jclass, jlong h) {
    auto* w = reinterpret_cast<JobWrap*>(h);
    if (!w || !w->job) return static_cast<jint>(ME_E_INVALID_ARG);
    return static_cast<jint>(me_render_wait(w->job));
}

JNIEXPORT void JNICALL
Java_io_mediaengine_MediaEngine_nativeRenderCancel(JNIEnv*, jclass, jlong h) {
    auto* w = reinterpret_cast<JobWrap*>(h);
    if (w && w->job) me_render_cancel(w->job);
}

JNIEXPORT void JNICALL
Java_io_mediaengine_MediaEngine_nativeRenderJobDestroy(JNIEnv* env, jclass, jlong h) {
    auto* w = reinterpret_cast<JobWrap*>(h);
    if (!w) return;
    if (w->job) me_render_job_destroy(w->job);
    if (w->cb) {
        if (w->cb->listener) env->DeleteGlobalRef(w->cb->listener);
        delete w->cb;
    }
    delete w;
}

JNIEXPORT jstring JNICALL
Java_io_mediaengine_MediaEngine_nativeLastError(JNIEnv* env, jclass, jlong h) {
    const char* msg = me_engine_last_error(reinterpret_cast<me_engine_t*>(h));
    return env->NewStringUTF(msg ? msg : "");
}

JNIEXPORT jobject JNICALL
Java_io_mediaengine_MediaEngine_nativeRenderFrame(JNIEnv* env, jclass,
                                                   jlong eng_h,
                                                   jlong tl_h,
                                                   jlong t_num,
                                                   jlong t_den) {
    auto* eng = reinterpret_cast<me_engine_t*>(eng_h);
    auto* tl  = reinterpret_cast<me_timeline_t*>(tl_h);
    me_rational_t t{static_cast<int64_t>(t_num),
                    static_cast<int64_t>(t_den)};
    me_frame_t* frame = nullptr;
    me_status_t s = me_render_frame(eng, tl, t, &frame);
    g_jni_last_status = s;
    if (s != ME_OK || !frame) {
        if (frame) me_frame_destroy(frame);
        return nullptr;
    }
    const int w = me_frame_width(frame);
    const int h = me_frame_height(frame);
    const std::uint8_t* px = me_frame_pixels(frame);
    if (w <= 0 || h <= 0 || !px) {
        me_frame_destroy(frame);
        return nullptr;
    }
    const std::size_t nbytes = static_cast<std::size_t>(w) *
                                static_cast<std::size_t>(h) * 4u;
    jbyteArray rgba = env->NewByteArray(static_cast<jsize>(nbytes));
    if (rgba) {
        env->SetByteArrayRegion(rgba, 0, static_cast<jsize>(nbytes),
                                 reinterpret_cast<const jbyte*>(px));
    }
    me_frame_destroy(frame);

    /* Construct MediaEngine.Frame(int width, int height, byte[] rgba). */
    jclass cls = env->FindClass("io/mediaengine/MediaEngine$Frame");
    if (!cls) return nullptr;
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(II[B)V");
    if (!ctor) { env->DeleteLocalRef(cls); return nullptr; }
    jobject obj = env->NewObject(cls, ctor,
                                  static_cast<jint>(w),
                                  static_cast<jint>(h),
                                  rgba);
    env->DeleteLocalRef(cls);
    return obj;
}

JNIEXPORT jbyteArray JNICALL
Java_io_mediaengine_MediaEngine_nativeThumbnail(JNIEnv* env, jclass,
                                                 jlong   eng_h,
                                                 jstring uri,
                                                 jlong   t_num,
                                                 jlong   t_den,
                                                 jint    max_width,
                                                 jint    max_height) {
    auto* eng = reinterpret_cast<me_engine_t*>(eng_h);
    const char* uri_c = env->GetStringUTFChars(uri, nullptr);
    me_rational_t t{static_cast<int64_t>(t_num),
                     static_cast<int64_t>(t_den)};
    uint8_t* png   = nullptr;
    size_t   nlen  = 0;
    me_status_t s = me_thumbnail_png(eng, uri_c, t, max_width, max_height,
                                      &png, &nlen);
    env->ReleaseStringUTFChars(uri, uri_c);
    g_jni_last_status = s;
    if (s != ME_OK || !png || nlen == 0) {
        if (png) me_buffer_free(png);
        return nullptr;
    }
    jbyteArray out = env->NewByteArray(static_cast<jsize>(nlen));
    if (out) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(nlen),
                                 reinterpret_cast<const jbyte*>(png));
    }
    me_buffer_free(png);
    return out;
}

JNIEXPORT jint JNICALL
Java_io_mediaengine_MediaEngine_nativeLastStatus(JNIEnv*, jclass) {
    /* Engine handle isn't passed because g_jni_last_status is
     * thread-local. Hosts that hop threads between a native* call
     * and lastStatus() must read the status on the same thread that
     * made the call — same discipline as me_engine_last_error. */
    return static_cast<jint>(g_jni_last_status);
}

JNIEXPORT jobject JNICALL
Java_io_mediaengine_MediaEngine_nativeVersion(JNIEnv* env, jclass) {
    const me_version_t v = me_version();
    jclass cls = env->FindClass("io/mediaengine/MediaEngine$Version");
    if (!cls) return nullptr;
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(IIILjava/lang/String;)V");
    if (!ctor) { env->DeleteLocalRef(cls); return nullptr; }
    jstring sha = env->NewStringUTF(v.git_sha ? v.git_sha : "");
    jobject obj = env->NewObject(cls, ctor,
                                  static_cast<jint>(v.major),
                                  static_cast<jint>(v.minor),
                                  static_cast<jint>(v.patch),
                                  sha);
    env->DeleteLocalRef(sha);
    env->DeleteLocalRef(cls);
    return obj;
}

}  /* extern "C" */
