/*
 * ConfigCacheAssert — pin the cycle 106 MediaEngine(Config)
 * ctor: passing a non-null cacheDir should engage the engine's
 * disk cache (verifiable via the cache_dir on disk + cache stat
 * counters that grow on the second me_render_frame at the same
 * time).
 *
 * Without this assertion, a regression that dropped the
 * cache_dir field on the JNI side would leave host hosts running
 * without disk cache + degraded scrub UX, with no test signal.
 *
 * Usage:
 *   java -Djava.library.path=<libdir> -cp <classes-dir> \
 *        io.mediaengine.example.ConfigCacheAssert \
 *        <source.mp4> <cache-dir>
 */
package io.mediaengine.example;

import io.mediaengine.MediaEngine;
import io.mediaengine.MediaEngine.Config;
import io.mediaengine.MediaEngine.Frame;
import io.mediaengine.MediaEngine.Timeline;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

public final class ConfigCacheAssert {

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: ConfigCacheAssert <source.mp4> <cache-dir>");
            System.exit(2);
        }
        final String source   = args[0];
        final Path   cacheDir = Paths.get(args[1]);
        Files.createDirectories(cacheDir);

        /* Construct with explicit Config: cacheDir set, no caps
         * (= unlimited disk cache). The disk cache populates on
         * the first frame fetch; subsequent fetches at the same
         * time hit it. */
        final Config cfg = new Config(cacheDir.toString(),
                                       /*memoryCacheBytes=*/0L,
                                       /*diskCacheBytes=*/0L);
        try (MediaEngine eng = new MediaEngine(cfg);
             Timeline tl    = eng.loadTimeline(
                     Timelines.passthrough("file://" + source))) {

            /* First fetch — populates the cache. */
            Frame f1 = eng.frame(tl, /*tNum=*/1L, /*tDen=*/2L);
            if (f1 == null) {
                System.err.println("first frame fetch failed: " + eng.lastError());
                System.exit(1);
            }

            /* Second fetch at the same time — should serve from
             * the cache populated by the first. */
            Frame f2 = eng.frame(tl, /*tNum=*/1L, /*tDen=*/2L);
            if (f2 == null) {
                System.err.println("second frame fetch failed: " + eng.lastError());
                System.exit(1);
            }

            /* Smoke proof of "disk cache engaged": at least one
             * file appeared under cacheDir. We don't enforce a
             * specific filename (DiskCache uses content-hash
             * keys); just check the dir is no longer empty. */
            final long cached = Files.list(cacheDir).count();
            System.out.printf("ConfigCacheAssert: cacheDir=%s files=%d%n",
                    cacheDir, cached);
            if (cached < 1) {
                System.err.println("disk cache directory empty after "
                        + "2 fetches — Config.cacheDir not honored");
                System.exit(1);
            }
            System.out.println("ConfigCacheAssert OK");
        }
    }
}
