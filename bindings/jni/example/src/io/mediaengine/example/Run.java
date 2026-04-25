/*
 * Run — minimal end-to-end JVM-side demo of the JNI binding.
 *
 * Wraps `MediaEngine` with an inline passthrough-timeline JSON
 * builder + a render_start → wait → close sequence. Talevia
 * integrators copy `MediaEngine.java` into their codebase but
 * leave this runner here as the canonical "what does a smoke
 * call look like" reference.
 *
 * Usage:
 *   java -Djava.library.path=<libdir> \
 *        -cp <classes-dir> \
 *        io.mediaengine.example.Run <source.mp4> <output.mp4>
 *
 * Exits 0 on success. Prints engine version, progress events,
 * and the final wait return code.
 */
package io.mediaengine.example;

import io.mediaengine.MediaEngine;
import io.mediaengine.MediaEngine.OutputSpec;
import io.mediaengine.MediaEngine.RenderJob;
import io.mediaengine.MediaEngine.Timeline;
import io.mediaengine.MediaEngine.Version;

import java.util.concurrent.atomic.AtomicInteger;

public final class Run {

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: Run <source.mp4> <output.mp4>");
            System.exit(2);
        }
        final String source = args[0];
        final String output = args[1];

        Version v = MediaEngine.version();
        System.out.println("media-engine " + v.major() + "." + v.minor()
                + "." + v.patch() + " (" + (v.gitSha().isEmpty() ? "<unknown>" : v.gitSha()) + ")");

        final String json = passthroughTimelineJson("file://" + source);

        /* Counts events per kind so the post-render assertion can pin
         * the trampoline contract: a successful render must deliver at
         * least one STARTED (kind 0) and at least one COMPLETED
         * (kind 2). A regression in progress_trampoline (e.g. failed
         * AttachCurrentThread, missing GetMethodID, callback swallowed
         * by ExceptionClear) would leave these counters at zero
         * without affecting the render rc — exit code stays 0 and the
         * test silently passes. The counter promotes that bug to a
         * test failure. */
        final AtomicInteger startedCount   = new AtomicInteger();
        final AtomicInteger framesCount    = new AtomicInteger();
        final AtomicInteger completedCount = new AtomicInteger();
        final AtomicInteger failedCount    = new AtomicInteger();

        try (MediaEngine eng = new MediaEngine();
             Timeline tl   = eng.loadTimeline(json)) {

            OutputSpec spec = new OutputSpec();
            spec.path        = output;
            /* Defaults: container=mp4, video/audio codec=passthrough.
             * Passthrough requires no encoder licensing, so this runs
             * cleanly against any FFmpeg build. */

            try (RenderJob job = eng.renderStart(tl, spec, (kind, ratio, msg) -> {
                /* Progress kinds: 0=STARTED 1=FRAMES 2=COMPLETED 3=FAILED. */
                if (kind == 0) {
                    startedCount.incrementAndGet();
                    System.out.println("started");
                } else if (kind == 1) {
                    framesCount.incrementAndGet();
                } else if (kind == 2) {
                    completedCount.incrementAndGet();
                    System.out.printf("done (%s)%n", msg == null ? "" : msg);
                } else if (kind == 3) {
                    failedCount.incrementAndGet();
                    System.err.println("failed: " + (msg == null ? "<no msg>" : msg));
                }
            })) {
                int rc = job.waitFor();
                System.out.println("render rc=" + rc);
                if (rc != 0) {
                    System.err.println("last error: " + eng.lastError());
                    System.exit(1);
                }
            }
        }

        /* Trampoline assertion: at least one STARTED and one COMPLETED
         * for a successful render. FRAMES count varies with fixture
         * length; not asserted. Any drop here means the JNI progress
         * path is broken even though the render itself succeeded — the
         * exact regression class this assertion exists to catch. */
        System.out.printf("progress counts: started=%d frames=%d completed=%d failed=%d%n",
                startedCount.get(), framesCount.get(),
                completedCount.get(), failedCount.get());
        if (startedCount.get() < 1 || completedCount.get() < 1) {
            System.err.println("progress trampoline regression: "
                    + "STARTED + COMPLETED both required, "
                    + "got started=" + startedCount.get()
                    + " completed=" + completedCount.get());
            System.exit(1);
        }
    }

    /* Build a single-clip passthrough timeline from `uri`. Container
     * defaults to mp4; the C side uses extension inference + the spec
     * codec strings to pick the passthrough copy path. Frame rate /
     * resolution are nominal here — passthrough copies the source's
     * stream params verbatim. */
    private static String passthroughTimelineJson(String uri) {
        return ""
            + "{\n"
            + "  \"schemaVersion\": 1,\n"
            + "  \"frameRate\":  {\"num\":30,\"den\":1},\n"
            + "  \"resolution\": {\"width\":160,\"height\":120},\n"
            + "  \"colorSpace\": {\"primaries\":\"bt709\",\"transfer\":\"bt709\","
                                + "\"matrix\":\"bt709\",\"range\":\"limited\"},\n"
            + "  \"assets\": [{\"id\":\"a0\",\"uri\":\"" + uri + "\"}],\n"
            + "  \"compositions\": [{\"id\":\"main\",\"tracks\":[{\n"
            + "    \"id\":\"v0\",\"kind\":\"video\",\"clips\":[{\n"
            + "      \"id\":\"c0\",\"type\":\"video\",\"assetId\":\"a0\",\n"
            + "      \"timeRange\":  {\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":2,\"den\":1}},\n"
            + "      \"sourceRange\":{\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":2,\"den\":1}}\n"
            + "    }]\n"
            + "  }]}],\n"
            + "  \"output\": {\"compositionId\":\"main\"}\n"
            + "}\n";
    }
}
