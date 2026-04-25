/*
 * ThumbnailBatch — JVM-side scrub-row pattern: fetch N PNG
 * thumbnails across the source's timeline, write each to disk.
 *
 * Real scrub UIs (talevia clip-card list, file picker preview)
 * fetch 10-50 thumbnails per source. Cycle 76's Thumbnail.java
 * demo only pulled one — this runner shows the loop shape +
 * surfaces engine-level cache behavior across requests via
 * MediaEngine.lastStatus() (cycle 84). Each call shares the same
 * engine handle, so the codec context + asset hash cache amortize
 * across the batch.
 *
 * Usage:
 *   java -Djava.library.path=<libdir> -cp <classes-dir> \
 *        io.mediaengine.example.ThumbnailBatch \
 *        <source.mp4> <out-dir>
 *
 * Writes <out-dir>/thumb_0.png ... thumb_4.png at 5 sample
 * times spread across the source's first 2 seconds.
 */
package io.mediaengine.example;

import io.mediaengine.MediaEngine;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

public final class ThumbnailBatch {

    /* 5 sample times: 0, 0.4, 0.8, 1.2, 1.6 seconds.
     * Pair (tNum, tDen) → 0/10, 4/10, 8/10, 12/10, 16/10. */
    private static final long[][] SAMPLES = {
        {0,  10}, {4, 10}, {8, 10}, {12, 10}, {16, 10},
    };

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: ThumbnailBatch <source.mp4> <out-dir>");
            System.exit(2);
        }
        final String source = args[0];
        final Path   outDir = Paths.get(args[1]);
        Files.createDirectories(outDir);

        try (MediaEngine eng = new MediaEngine()) {
            int wrote = 0;
            for (int i = 0; i < SAMPLES.length; ++i) {
                final long tNum = SAMPLES[i][0];
                final long tDen = SAMPLES[i][1];
                byte[] png = eng.thumbnail("file://" + source,
                                           tNum, tDen,
                                           /*maxWidth=*/160, /*maxHeight=*/120);
                if (png == null || png.length == 0) {
                    System.err.printf("thumbnail[%d] (t=%d/%d) failed: %s "
                                    + "(status=%d)%n",
                            i, tNum, tDen, eng.lastError(),
                            MediaEngine.lastStatus());
                    continue;
                }
                final Path outPath = outDir.resolve("thumb_" + i + ".png");
                Files.write(outPath, png);
                System.out.printf("  thumb_%d (t=%.2fs) → %s (%d bytes)%n",
                        i, (double) tNum / (double) tDen,
                        outPath, png.length);
                ++wrote;
            }
            System.out.printf("wrote %d/%d thumbnails%n",
                    wrote, SAMPLES.length);
            if (wrote < SAMPLES.length) {
                System.exit(1);
            }
        }
    }
}
