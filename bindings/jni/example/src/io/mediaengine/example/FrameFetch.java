/*
 * FrameFetch — JVM-side demo of me_render_frame via the
 * MediaEngine.frame(Timeline, tNum, tDen) bridge. Builds a
 * single-clip passthrough timeline over <source.mp4>, fetches
 * the frame at t=1/2 (0.5 s), writes a PPM to <output.ppm>.
 *
 * Hosts (talevia scrub UI, agent preview) need raw RGBA frames —
 * thumbnail() is the PNG-encoded path; this is the
 * decoded-pixels path that doesn't pay for PNG encoding.
 *
 * Usage:
 *   java -Djava.library.path=<libdir> -cp <classes-dir> \
 *        io.mediaengine.example.FrameFetch \
 *        <source.mp4> <output.ppm>
 */
package io.mediaengine.example;

import io.mediaengine.MediaEngine;
import io.mediaengine.MediaEngine.Frame;
import io.mediaengine.MediaEngine.Timeline;

import java.io.OutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Paths;

public final class FrameFetch {

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: FrameFetch <source.mp4> <output.ppm>");
            System.exit(2);
        }
        final String source = args[0];
        final String output = args[1];

        final String json = Timelines.passthrough("file://" + source);

        try (MediaEngine eng = new MediaEngine();
             Timeline tl    = eng.loadTimeline(json)) {

            Frame f = eng.frame(tl, /*tNum=*/1L, /*tDen=*/2L);
            if (f == null) {
                System.err.println("frame fetch failed: " + eng.lastError()
                        + " (status=" + MediaEngine.lastStatus() + ")");
                System.exit(1);
            }
            try (OutputStream out = Files.newOutputStream(Paths.get(output))) {
                writePpm(out, f);
            }
            System.out.printf("wrote %s (%dx%d, %d bytes RGBA)%n",
                    output, f.width(), f.height(), f.rgba().length);
        }
    }

    /* RGBA8 → P6 PPM. Skip the alpha channel (PPM is RGB). */
    private static void writePpm(OutputStream out, Frame f) throws IOException {
        final String header = "P6\n" + f.width() + " " + f.height() + "\n255\n";
        out.write(header.getBytes());
        final byte[] rgba = f.rgba();
        final byte[] row = new byte[f.width() * 3];
        for (int y = 0; y < f.height(); ++y) {
            final int rowStart = y * f.width() * 4;
            for (int x = 0; x < f.width(); ++x) {
                row[x * 3 + 0] = rgba[rowStart + x * 4 + 0];
                row[x * 3 + 1] = rgba[rowStart + x * 4 + 1];
                row[x * 3 + 2] = rgba[rowStart + x * 4 + 2];
            }
            out.write(row);
        }
    }
}
