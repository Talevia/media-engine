/*
 * Thumbnail — JVM-side demo of the me_thumbnail_png JNI bridge.
 *
 * Hosts (talevia, scrub-row UIs) need PNG thumbnails to populate
 * file pickers, timeline rulers, and clip cards. The wrapper
 * exposes `MediaEngine.thumbnail(uri, tNum, tDen, w, h)` returning
 * a PNG byte array; this demo writes the result to disk.
 *
 * Usage:
 *   java -Djava.library.path=<libdir> \
 *        -cp <classes-dir> \
 *        io.mediaengine.example.Thumbnail \
 *        <source.mp4> <output.png>
 *
 * Pulls a frame at t=1/2 (0.5 s) and writes a 160x120 PNG.
 */
package io.mediaengine.example;

import io.mediaengine.MediaEngine;

import java.nio.file.Files;
import java.nio.file.Paths;

public final class Thumbnail {

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: Thumbnail <source.mp4> <output.png>");
            System.exit(2);
        }
        final String source = args[0];
        final String output = args[1];

        try (MediaEngine eng = new MediaEngine()) {
            byte[] png = eng.thumbnail("file://" + source,
                                       /*tNum=*/1L, /*tDen=*/2L,
                                       /*maxWidth=*/160, /*maxHeight=*/120);
            if (png == null || png.length == 0) {
                System.err.println("thumbnail failed: " + eng.lastError());
                System.exit(1);
            }
            Files.write(Paths.get(output), png);
            System.out.printf("wrote %s (%d bytes)%n", output, png.length);
        }
    }
}
