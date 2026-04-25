/*
 * ProgressThrow — pin the JNI progress trampoline's
 * ExceptionClear path. me_jni.cpp:74 catches + swallows
 * exceptions thrown by the Java ProgressListener so the engine
 * worker keeps running; the comment promises "callback impls
 * shouldn't throw, but if they do, swallow to keep the engine
 * alive". No prior test exercised that path — a regression that
 * dropped the ExceptionCheck (or replaced it with abort) would
 * crash hosts the moment a listener bug throws.
 *
 * Strategy: install a listener that throws on the 2nd FRAMES
 * event then behaves normally. Assert (a) render completes
 * with rc=0 (engine survived the throw), (b) STARTED + at
 * least one FRAMES + COMPLETED were observed (events keep
 * flowing after the throw), (c) the throw counter went up
 * (proves the path was actually exercised).
 *
 * Usage:
 *   java -Djava.library.path=<libdir> -cp <classes-dir> \
 *        io.mediaengine.example.ProgressThrow \
 *        <source.mp4> <output.mp4>
 */
package io.mediaengine.example;

import io.mediaengine.MediaEngine;
import io.mediaengine.MediaEngine.OutputSpec;
import io.mediaengine.MediaEngine.RenderJob;
import io.mediaengine.MediaEngine.Timeline;

import java.util.concurrent.atomic.AtomicInteger;

public final class ProgressThrow {

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: ProgressThrow <source.mp4> <output.mp4>");
            System.exit(2);
        }
        final String source = args[0];
        final String output = args[1];

        final String json = ""
            + "{\n"
            + "  \"schemaVersion\": 1,\n"
            + "  \"frameRate\":  {\"num\":30,\"den\":1},\n"
            + "  \"resolution\": {\"width\":160,\"height\":120},\n"
            + "  \"colorSpace\": {\"primaries\":\"bt709\",\"transfer\":\"bt709\","
                                + "\"matrix\":\"bt709\",\"range\":\"limited\"},\n"
            + "  \"assets\": [{\"id\":\"a0\",\"uri\":\"file://" + source + "\"}],\n"
            + "  \"compositions\": [{\"id\":\"main\",\"tracks\":[{\n"
            + "    \"id\":\"v0\",\"kind\":\"video\",\"clips\":[{\n"
            + "      \"id\":\"c0\",\"type\":\"video\",\"assetId\":\"a0\",\n"
            + "      \"timeRange\":  {\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":2,\"den\":1}},\n"
            + "      \"sourceRange\":{\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":2,\"den\":1}}\n"
            + "    }]\n"
            + "  }]}],\n"
            + "  \"output\": {\"compositionId\":\"main\"}\n"
            + "}\n";

        final AtomicInteger framesSeen     = new AtomicInteger();
        final AtomicInteger startedSeen    = new AtomicInteger();
        final AtomicInteger completedSeen  = new AtomicInteger();
        final AtomicInteger threwCount     = new AtomicInteger();

        try (MediaEngine eng = new MediaEngine();
             Timeline tl    = eng.loadTimeline(json)) {

            OutputSpec spec = new OutputSpec();
            spec.path = output;

            try (RenderJob job = eng.renderStart(tl, spec, (kind, ratio, msg) -> {
                if (kind == 0) startedSeen.incrementAndGet();
                else if (kind == 1) {
                    final int n = framesSeen.incrementAndGet();
                    if (n == 2) {
                        threwCount.incrementAndGet();
                        throw new RuntimeException("intentional listener throw on frame 2");
                    }
                } else if (kind == 2) completedSeen.incrementAndGet();
            })) {
                int rc = job.waitFor();
                System.out.printf("rc=%d started=%d frames=%d completed=%d threw=%d%n",
                        rc, startedSeen.get(), framesSeen.get(),
                        completedSeen.get(), threwCount.get());

                if (rc != 0) {
                    System.err.println("render failed despite trampoline ExceptionClear: "
                            + eng.lastError());
                    System.exit(1);
                }
                if (threwCount.get() < 1) {
                    System.err.println("listener never reached frame 2 — test fixture too short");
                    System.exit(1);
                }
                /* The crucial assertion: a frame AFTER the throw must
                 * have been delivered. If the trampoline aborted the
                 * worker on the throw, frames_seen would equal 2 + no
                 * COMPLETED. */
                if (framesSeen.get() < 3) {
                    System.err.println("trampoline failed to recover after listener throw "
                            + "(no frames delivered post-throw)");
                    System.exit(1);
                }
                if (completedSeen.get() < 1) {
                    System.err.println("trampoline failed to deliver COMPLETED after throw");
                    System.exit(1);
                }
                System.out.println("ProgressThrow OK");
            }
        }
    }
}
