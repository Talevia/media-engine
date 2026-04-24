/*
 * media-engine JVM binding — thin JNI wrapper mirroring the C API
 * surface hosts need for a smoke-level integration (create / destroy
 * engine + timeline, run a render job, observe progress).
 *
 * Host contract:
 *   - `System.loadLibrary("media_engine_jni")` pulls
 *     libmedia_engine_jni.{dylib,so,dll} from java.library.path.
 *     libmedia_engine.{dylib,so} must already be reachable from the
 *     same path (it's a transitive dependency of the JNI lib).
 *   - All handles (engine, timeline, renderJob) implement
 *     AutoCloseable so try-with-resources is the canonical idiom.
 *   - Native methods may return `0` to signal failure; the wrapper
 *     translates that into a RuntimeException carrying the last
 *     engine error message.
 *
 * Not intended to be a full idiomatic Java API — hosts layer their
 * own abstractions (coroutines / Flow / dependency-injected sinks)
 * on top. See docs/INTEGRATION.md → "JNI (JVM, Android)".
 */
package io.mediaengine;

public final class MediaEngine implements AutoCloseable {

    static {
        System.loadLibrary("media_engine_jni");
    }

    private final long handle;

    public MediaEngine() {
        this.handle = nativeCreate();
        if (handle == 0L) {
            throw new RuntimeException("me_engine_create failed");
        }
    }

    public Timeline loadTimeline(String json) {
        long h = nativeLoadTimeline(handle, json);
        if (h == 0L) {
            throw new RuntimeException("me_timeline_load_json failed: " + nativeLastError(handle));
        }
        return new Timeline(h);
    }

    public RenderJob renderStart(Timeline tl, OutputSpec spec, ProgressListener listener) {
        long jh = nativeRenderStart(
                handle, tl.handle,
                spec.path, spec.container, spec.videoCodec, spec.audioCodec,
                spec.videoBitrateBps, spec.audioBitrateBps,
                spec.width, spec.height,
                spec.frameRateNum, spec.frameRateDen,
                listener);
        if (jh == 0L) {
            throw new RuntimeException("me_render_start failed: " + nativeLastError(handle));
        }
        return new RenderJob(jh);
    }

    public String lastError() {
        return nativeLastError(handle);
    }

    @Override
    public void close() {
        nativeDestroy(handle);
    }

    /** Build-time version metadata (major.minor.patch + git SHA). */
    public static Version version() {
        return nativeVersion();
    }

    /** Output configuration — fields mirror the C `me_output_spec_t`. */
    public static final class OutputSpec {
        public String path;
        public String container  = "mp4";
        public String videoCodec = "passthrough";
        public String audioCodec = "passthrough";
        public long   videoBitrateBps = 0L;
        public long   audioBitrateBps = 0L;
        public int    width  = 0;
        public int    height = 0;
        public long   frameRateNum = 0L;
        public long   frameRateDen = 1L;
    }

    /** Opaque timeline handle — close when done. */
    public static final class Timeline implements AutoCloseable {
        final long handle;
        Timeline(long h) { this.handle = h; }
        @Override public void close() { nativeTimelineDestroy(handle); }
    }

    /** Async render-job handle — wait() or cancel(); close() releases the underlying job. */
    public final class RenderJob implements AutoCloseable {
        private final long jobHandle;
        RenderJob(long h) { this.jobHandle = h; }
        public int waitFor()   { return nativeRenderWait(jobHandle); }
        public void cancel()   { nativeRenderCancel(jobHandle); }
        @Override public void close() { nativeRenderJobDestroy(jobHandle); }
    }

    /** Terminal kinds: 0=STARTED 1=FRAMES 2=COMPLETED 3=FAILED, matching me_progress_kind_t. */
    @FunctionalInterface
    public interface ProgressListener {
        void onProgress(int kind, float ratio, String message);
    }

    public record Version(int major, int minor, int patch, String gitSha) {}

    /* ------------------------------------------------------------------
     * Native bridges — implementations in me_jni.cpp. */
    private static native long    nativeCreate();
    private static native void    nativeDestroy(long engine);
    private static native long    nativeLoadTimeline(long engine, String json);
    private static native void    nativeTimelineDestroy(long tl);
    private static native long    nativeRenderStart(
            long engine, long tl,
            String path, String container, String videoCodec, String audioCodec,
            long videoBitrateBps, long audioBitrateBps,
            int width, int height,
            long frameRateNum, long frameRateDen,
            ProgressListener listener);
    private static native int     nativeRenderWait(long job);
    private static native void    nativeRenderCancel(long job);
    private static native void    nativeRenderJobDestroy(long job);
    private static native String  nativeLastError(long engine);
    private static native Version nativeVersion();

    /* ------------------------------------------------------------------
     * Demo entry point:
     *   java -Djava.library.path=<build-dir> io.mediaengine.MediaEngine \
     *        <timeline.json> <output.mp4>
     * With no args, just prints the engine version and round-trips a
     * handle. */
    public static void main(String[] args) throws Exception {
        Version v = version();
        System.out.println("media-engine " + v.major + "." + v.minor + "." + v.patch
                + " (" + (v.gitSha.isEmpty() ? "<unknown>" : v.gitSha) + ")");

        try (MediaEngine eng = new MediaEngine()) {
            System.out.println("engine created: 0x" + Long.toHexString(eng.handle));

            if (args.length >= 1) {
                String json = new String(
                        java.nio.file.Files.readAllBytes(java.nio.file.Paths.get(args[0])),
                        java.nio.charset.StandardCharsets.UTF_8);
                try (Timeline tl = eng.loadTimeline(json)) {
                    System.out.println("timeline loaded");
                    if (args.length >= 2) {
                        OutputSpec spec = new OutputSpec();
                        spec.path = args[1];
                        try (RenderJob job = eng.renderStart(tl, spec, (kind, ratio, msg) ->
                                System.out.println("progress kind=" + kind
                                        + " ratio=" + ratio
                                        + (msg != null && !msg.isEmpty() ? " msg=" + msg : "")))) {
                            int rc = job.waitFor();
                            System.out.println("render done rc=" + rc);
                        }
                    }
                }
            } else {
                System.out.println("no timeline path — version + handle round-trip only");
            }
        }
    }
}
