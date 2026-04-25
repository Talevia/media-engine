/*
 * Cancel — JVM-side smoke for nativeRenderCancel.
 *
 * Run.java only exercises the start → wait-to-completion path;
 * Thumbnail.java doesn't touch render at all. A regression in the
 * cancel path (forgotten Detach on the worker thread, deadlock
 * between cancel + the in-flight encoder loop) wouldn't surface
 * until talevia integrates a "Stop render" button.
 *
 * This runner starts a passthrough render, sleeps briefly to let
 * the engine actually enter the encode loop, calls cancel(), then
 * waits. Exit 0 on either:
 *   - rc == 0 (render finished before cancel landed — race-OK on
 *              very short fixtures), or
 *   - rc == -8 (ME_E_CANCELLED — cancel landed mid-flight).
 * Anything else (positive rc, hang, throw) fails the ctest.
 *
 * Usage:
 *   java -Djava.library.path=<libdir> \
 *        -cp <classes-dir> \
 *        io.mediaengine.example.Cancel \
 *        <source.mp4> <output.mp4>
 */
package io.mediaengine.example;

import io.mediaengine.MediaEngine;
import io.mediaengine.MediaEngine.OutputSpec;
import io.mediaengine.MediaEngine.RenderJob;
import io.mediaengine.MediaEngine.Timeline;

public final class Cancel {

    /* Mirrors me_status_t::ME_E_CANCELLED (-8 in types.h). */
    private static final int ME_E_CANCELLED = -8;

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: Cancel <source.mp4> <output.mp4>");
            System.exit(2);
        }
        final String source = args[0];
        final String output = args[1];

        final String json = Timelines.passthrough("file://" + source);

        try (MediaEngine eng = new MediaEngine();
             Timeline tl    = eng.loadTimeline(json)) {

            OutputSpec spec = new OutputSpec();
            spec.path = output;

            try (RenderJob job = eng.renderStart(tl, spec, null)) {
                /* Give the worker thread a few ms to enter the encode
                 * loop. Sleep is intentionally short — passthrough
                 * copies are fast and waiting longer makes the rc==0
                 * branch dominate. */
                Thread.sleep(5);
                job.cancel();
                int rc = job.waitFor();
                System.out.println("rc=" + rc);
                if (rc != 0 && rc != ME_E_CANCELLED) {
                    System.err.println("unexpected rc " + rc + ": " + eng.lastError());
                    System.exit(1);
                }
            }
        }
    }
}
