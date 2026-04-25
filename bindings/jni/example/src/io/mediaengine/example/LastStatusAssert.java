/*
 * LastStatusAssert — pin nativeLastStatus + MediaEngineException
 * status-mapping. Loads malformed JSON, expects MediaEngineException,
 * and asserts the caught exception's structured `status` field
 * equals ME_E_PARSE (-5). Without this assertion, a regression that
 * forgot to capture g_jni_last_status before nativeLoadTimeline
 * returned 0 would leave Java seeing status=0 (== ME_OK) and
 * hosts couldn't reliably distinguish parse / IO / unsupported
 * failure modes.
 *
 * Usage:
 *   java -Djava.library.path=<libdir> -cp <classes-dir> \
 *        io.mediaengine.example.LastStatusAssert
 *
 * No args. Exits 0 on success; 1 on any contract violation.
 */
package io.mediaengine.example;

import io.mediaengine.MediaEngine;

public final class LastStatusAssert {

    /* From include/media_engine/types.h. */
    private static final int ME_E_PARSE = -4;

    public static void main(String[] args) {
        try (MediaEngine eng = new MediaEngine()) {
            try {
                eng.loadTimeline("not-json");
                System.err.println("expected MediaEngineException on malformed JSON");
                System.exit(1);
            } catch (MediaEngine.MediaEngineException ex) {
                System.out.printf("caught: status=%d msg=%s%n", ex.status, ex.getMessage());
                if (ex.status != ME_E_PARSE) {
                    System.err.printf("expected status %d (ME_E_PARSE), got %d%n",
                            ME_E_PARSE, ex.status);
                    System.exit(1);
                }
            }

            /* MediaEngine.lastStatus() also reports the same code on
             * the calling thread — pin that contract too so callers
             * have two equivalent paths (catch the exception OR
             * read the static accessor). */
            int s = MediaEngine.lastStatus();
            if (s != ME_E_PARSE) {
                System.err.printf("MediaEngine.lastStatus() = %d, expected %d%n",
                        s, ME_E_PARSE);
                System.exit(1);
            }

            System.out.println("LastStatusAssert OK");
        }
    }
}
