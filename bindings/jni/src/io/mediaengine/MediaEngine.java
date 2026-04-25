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
            throw new MediaEngineException(nativeLastStatus(),
                    "me_engine_create failed");
        }
    }

    /** Construct an engine with explicit configuration. Mirrors
     *  {@code me_engine_config_t} fields: a non-null {@code cacheDir}
     *  enables the disk cache (see VISION §3.3 cache contract), and
     *  the cap fields can override engine defaults. Pass
     *  {@code null} / 0 to fall back to defaults equivalent to the
     *  no-arg constructor's behavior. */
    public MediaEngine(Config cfg) {
        this.handle = nativeCreateWithConfig(
                cfg == null ? null : cfg.cacheDir(),
                cfg == null ? 0L   : cfg.memoryCacheBytes(),
                cfg == null ? 0L   : cfg.diskCacheBytes());
        if (handle == 0L) {
            throw new MediaEngineException(nativeLastStatus(),
                    "me_engine_create(Config) failed");
        }
    }

    public Timeline loadTimeline(String json) {
        long h = nativeLoadTimeline(handle, json);
        if (h == 0L) {
            throw new MediaEngineException(nativeLastStatus(),
                    "me_timeline_load_json failed: " + nativeLastError(handle));
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
            throw new MediaEngineException(nativeLastStatus(),
                    "me_render_start failed: " + nativeLastError(handle));
        }
        return new RenderJob(jh);
    }

    public String lastError() {
        return nativeLastError(handle);
    }

    /** Last me_status_t captured by the JNI bridge on the calling
     *  thread. Mirrors the values in include/media_engine/types.h
     *  (ME_OK=0, ME_E_INVALID_ARG=-1, ME_E_OUT_OF_MEMORY=-2,
     *  ME_E_IO=-3, ME_E_PARSE=-4, ME_E_DECODE=-5, ME_E_ENCODE=-6,
     *  ME_E_UNSUPPORTED=-7, ME_E_CANCELLED=-8, ME_E_NOT_FOUND=-9,
     *  ME_E_INTERNAL=-100).
     *
     *  Hosts use this to make graceful retry-vs-bail decisions
     *  without parsing the lastError() string (e.g. retry on
     *  ME_E_IO, bail on ME_E_PARSE / ME_E_INVALID_ARG). */
    public static int lastStatus() {
        return nativeLastStatus();
    }

    /** Render a single frame at `tNum/tDen` from `uri` as a PNG byte array.
     *  `maxWidth`/`maxHeight` of 0 = native dimensions. Returns null on
     *  failure (caller can read lastError() for details). */
    public byte[] thumbnail(String uri, long tNum, long tDen,
                            int maxWidth, int maxHeight) {
        return nativeThumbnail(handle, uri, tNum, tDen, maxWidth, maxHeight);
    }

    /** Render a single RGBA8 frame from `tl` at timeline time `tNum/tDen`.
     *  Returns null on failure (caller reads lastStatus()/lastError()).
     *  The returned Frame.rgba is a freshly-allocated copy — the engine's
     *  underlying me_frame_t is destroyed before this method returns,
     *  so hosts may retain Frame indefinitely. */
    public Frame frame(Timeline tl, long tNum, long tDen) {
        return nativeRenderFrame(handle, tl.handle, tNum, tDen);
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

    /** Engine construction config, mirroring {@code me_engine_config_t}.
     *
     *  <p><b>cacheDir.</b> Non-null enables the disk cache (cycle 39):
     *  intermediate frame results land under this directory keyed by
     *  content hash. Null leaves the disk cache disabled.
     *
     *  <p><b>memoryCacheBytes.</b> In-memory frame cache cap, bytes.
     *  0 = engine default.
     *
     *  <p><b>diskCacheBytes.</b> Disk cache cap, bytes. Applies only
     *  when {@code cacheDir} is set. 0 = unlimited (no eviction;
     *  directory grows until cleared / filesystem fills). When
     *  positive, the engine evicts oldest-by-mtime entries to keep
     *  the on-disk footprint under the cap. */
    public record Config(String cacheDir,
                          long   memoryCacheBytes,
                          long   diskCacheBytes) {}

    /** Decoded frame returned by {@link #frame(Timeline,long,long)}.
     *
     *  <p><b>Pixel layout.</b> RGBA8 row-major. Each pixel is exactly
     *  4 bytes in {R, G, B, A} order — byte 0 is the red channel,
     *  byte 3 is the alpha channel. There is <em>no padding</em>
     *  between pixels or rows; stride is always {@code width * 4}.
     *  Total array length is {@code width * height * 4}.
     *
     *  <p><b>Pixel access.</b> To read pixel {@code (x, y)} (origin
     *  top-left, x grows right, y grows down):
     *  <pre>{@code
     *    int i = (y * frame.width() + x) * 4;
     *    int r = rgba[i]     & 0xFF;
     *    int g = rgba[i + 1] & 0xFF;
     *    int b = rgba[i + 2] & 0xFF;
     *    int a = rgba[i + 3] & 0xFF;
     *  }</pre>
     *  Java's {@code byte} is signed, so the {@code & 0xFF} mask is
     *  required when widening to int. To convert to a JavaFX
     *  {@code WritableImage} or AWT {@code BufferedImage} of type
     *  {@code TYPE_4BYTE_ABGR}, byte-swap channels (R↔A, G↔B) per
     *  pixel; the C side delivers RGBA, not ABGR.
     *
     *  <p><b>Color space.</b> The engine targets the timeline's
     *  declared {@code colorSpace} field (see TIMELINE_SCHEMA.md);
     *  by convention sRGB primaries + sRGB transfer for SDR Rec.709
     *  output. HDR (PQ / HLG) returns linear-light values when
     *  enabled by the timeline. The Frame record itself doesn't
     *  carry color-space metadata — hosts that mix outputs from
     *  multiple timelines should track it on their side.
     *
     *  <p><b>Lifetime.</b> The {@code byte[]} is a Java-managed
     *  copy of the engine's underlying {@code me_frame_t} pixel
     *  buffer (destroyed before this record is constructed). Hosts
     *  may retain Frame indefinitely; no JNI handle is held. */
    public record Frame(int width, int height, byte[] rgba) {}

    /** Thrown by the wrapper when a native* call returns a failure
     *  sentinel. Carries the structured me_status_t code so hosts
     *  can `switch (e.status)` on retry logic instead of
     *  string-matching the message.  */
    public static final class MediaEngineException extends RuntimeException {
        public final int status;
        MediaEngineException(int status, String message) {
            super(message);
            this.status = status;
        }
    }

    /* ------------------------------------------------------------------
     * Native bridges — implementations in me_jni.cpp. */
    private static native long    nativeCreate();
    private static native long    nativeCreateWithConfig(
            String cacheDir, long memoryCacheBytes, long diskCacheBytes);
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
    private static native int     nativeLastStatus();
    private static native Version nativeVersion();
    private static native byte[]  nativeThumbnail(
            long engine, String uri,
            long tNum, long tDen,
            int maxWidth, int maxHeight);
    private static native Frame   nativeRenderFrame(
            long engine, long timeline, long tNum, long tDen);

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
