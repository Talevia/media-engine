/*
 * Frames — Java-side helpers for `MediaEngine.Frame`. Optional layer
 * on top of the binding: hosts that don't need AWT (Android, headless
 * services) can ignore this class entirely. Importing it pulls in
 * `java.awt.image.BufferedImage`; importing only `MediaEngine` does
 * not.
 *
 * Currently exposes one helper: `toBufferedImage`, which packs the
 * RGBA8 row-major bytes that the engine produces into a
 * `TYPE_INT_ARGB` BufferedImage suitable for `ImageIO.write`,
 * Swing painting, drag-into-clipboard, etc.
 */
package io.mediaengine;

import io.mediaengine.MediaEngine.Frame;

import java.awt.image.BufferedImage;
import java.awt.image.DataBufferInt;

public final class Frames {

    private Frames() {}   /* utility class — do not instantiate */

    /** Repackage a {@link Frame} (RGBA8 row-major; see Frame's
     *  Javadoc for the byte layout) into a
     *  {@link BufferedImage} of type {@link BufferedImage#TYPE_INT_ARGB}.
     *
     *  <p><b>Channel mapping.</b> The C side delivers bytes in
     *  {@code R, G, B, A} order; {@code TYPE_INT_ARGB} stores each
     *  pixel as a single 32-bit int with channels packed
     *  {@code (A << 24) | (R << 16) | (G << 8) | B}. This helper
     *  performs that pack per pixel and writes directly into the
     *  image's backing {@link DataBufferInt} — no intermediate
     *  byte[]/int[] allocation, no row-by-row copy.
     *
     *  <p><b>Why TYPE_INT_ARGB and not TYPE_4BYTE_ABGR.</b> Both
     *  represent 32-bit ARGB8; TYPE_INT_ARGB is the canonical
     *  choice for image sources that originate as packed ints
     *  (engine output is one int per pixel after this helper),
     *  while TYPE_4BYTE_ABGR matches AWT's native scanline format
     *  on some platforms but requires the channel byte-swap the
     *  engine's own RGBA layout doesn't already satisfy. Hosts
     *  that specifically need a TYPE_4BYTE_ABGR image can call
     *  {@link BufferedImage#getGraphics} to draw the result of
     *  this helper into one.
     *
     *  <p><b>Lifetime.</b> The returned BufferedImage owns a fresh
     *  pixel buffer; mutating the source {@code Frame.rgba()} does
     *  not affect it. The Frame's underlying byte array is read
     *  once during the pack.
     *
     *  @param frame source Frame; non-null, with width × height × 4
     *               == rgba.length per the binding contract.
     *  @return RGBA8 → TYPE_INT_ARGB BufferedImage, ready for
     *          ImageIO / Graphics2D / WritableImage adaptation.
     *  @throws NullPointerException if {@code frame} or its
     *          {@code rgba()} is null.
     *  @throws IllegalArgumentException if width / height / rgba.length
     *          are inconsistent (rgba.length != width * height * 4)
     *          or any dimension is non-positive — those would
     *          indicate a malformed Frame from the C side. */
    public static BufferedImage toBufferedImage(Frame frame) {
        java.util.Objects.requireNonNull(frame, "frame");
        java.util.Objects.requireNonNull(frame.rgba(), "frame.rgba()");

        final int w = frame.width();
        final int h = frame.height();
        final byte[] rgba = frame.rgba();
        if (w <= 0 || h <= 0) {
            throw new IllegalArgumentException(
                    "Frames.toBufferedImage: non-positive dimensions "
                    + w + "×" + h);
        }
        if ((long) rgba.length != (long) w * h * 4) {
            throw new IllegalArgumentException(
                    "Frames.toBufferedImage: rgba.length=" + rgba.length
                    + " != width*height*4 (" + (w * h * 4) + ")");
        }

        BufferedImage img = new BufferedImage(w, h, BufferedImage.TYPE_INT_ARGB);
        int[] argb = ((DataBufferInt) img.getRaster().getDataBuffer()).getData();

        /* Pack RGBA8 → ARGB int per pixel. byte → int is signed in
         * Java, so each channel needs `& 0xff`. */
        for (int i = 0, p = 0; p < argb.length; ++p, i += 4) {
            final int r = rgba[i    ] & 0xff;
            final int g = rgba[i + 1] & 0xff;
            final int b = rgba[i + 2] & 0xff;
            final int a = rgba[i + 3] & 0xff;
            argb[p] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        return img;
    }
}
