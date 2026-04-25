/*
 * ThumbnailArgsAssert — pin the cycle 107 contract that
 * MediaEngine.thumbnail rejects negative maxWidth / maxHeight
 * with IllegalArgumentException. The C side's me_thumbnail_png
 * treats negatives as undefined; the JVM wrapper guards at the
 * bridge boundary so hosts get a typed exception instead of
 * a corrupt byte[] (or worse, a swscale crash deeper in the
 * stack).
 *
 * Each subcase tries one bad arg pair, expects the throw, and
 * exits 1 if the wrapper accepted the call instead.
 *
 * Usage:
 *   java -Djava.library.path=<libdir> -cp <classes-dir> \
 *        io.mediaengine.example.ThumbnailArgsAssert
 *
 * No source URI needed — the bad-arg check happens in Java
 * before the JNI bridge fires.
 */
package io.mediaengine.example;

import io.mediaengine.MediaEngine;

public final class ThumbnailArgsAssert {

    public static void main(String[] args) {
        try (MediaEngine eng = new MediaEngine()) {
            int failures = 0;
            failures += expectThrow(
                "negative maxWidth",
                () -> eng.thumbnail("file:///dev/null", 0L, 1L, -1, 120));
            failures += expectThrow(
                "negative maxHeight",
                () -> eng.thumbnail("file:///dev/null", 0L, 1L, 160, -1));
            failures += expectThrow(
                "both negative",
                () -> eng.thumbnail("file:///dev/null", 0L, 1L, -100, -100));

            /* Sanity check: the wrapper should NOT throw on 0
             * (= native dimensions; the documented sentinel). */
            try {
                /* file:///dev/null returns null bytes (no PNG path)
                 * but doesn't throw — verifies the IAE check is
                 * specifically negative-value, not "any non-positive". */
                eng.thumbnail("file:///dev/null", 0L, 1L, 0, 0);
            } catch (IllegalArgumentException ex) {
                System.err.println("regression: maxWidth/Height=0 threw IAE: "
                        + ex.getMessage());
                ++failures;
            }

            if (failures > 0) {
                System.err.println(failures + " expected reject(s) did not match");
                System.exit(1);
            }
            System.out.println("ThumbnailArgsAssert OK");
        }
    }

    @FunctionalInterface
    private interface Call { void run(); }

    private static int expectThrow(String label, Call c) {
        try {
            c.run();
        } catch (IllegalArgumentException ex) {
            System.out.printf("  %s → caught IAE: %s%n", label, ex.getMessage());
            return 0;
        }
        System.err.printf("  %s → no exception thrown (expected IAE)%n", label);
        return 1;
    }
}
