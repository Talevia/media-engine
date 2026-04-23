/*
 * me::compose::affine_blit impl. See affine_blit.hpp for contract.
 */
#include "compose/affine_blit.hpp"

#include <cmath>
#include <cstring>

namespace me::compose {

namespace {

constexpr double PI = 3.14159265358979323846;

}  // namespace

AffineMatrix compose_inverse_affine(double translate_x,
                                     double translate_y,
                                     double scale_x,
                                     double scale_y,
                                     double rotation_deg,
                                     double anchor_x,
                                     double anchor_y,
                                     int    src_w,
                                     int    src_h) {
    AffineMatrix inv;
    if (scale_x == 0.0 || scale_y == 0.0) {
        /* Singular — return identity; output will be transparent
         * where transform was invalid. */
        return inv;
    }

    const double theta = rotation_deg * (PI / 180.0);
    const double c = std::cos(theta);
    const double s = std::sin(theta);

    /* Forward transform src (sx,sy) → canvas (cx,cy):
     *   p  = (sx - anchor_x*src_w, sy - anchor_y*src_h)
     *   p' = Scale * p
     *   p'' = Rotate(theta) * p'
     *   canvas = p'' + (anchor_x*src_w, anchor_y*src_h) + (translate_x, translate_y)
     *
     * Inverse (canvas → src):
     *   q = canvas - (translate_x + anchor_x*src_w, translate_y + anchor_y*src_h)
     *   q' = Rotate(-theta) * q
     *   q'' = InvScale * q'
     *   src = q'' + (anchor_x*src_w, anchor_y*src_h)
     *
     * Rotate(-theta) = [c s; -s c] (clockwise θ → inverse is ccw θ
     * which in a Y-down image frame is "rotate by -θ with s negated
     * one way" — we mirror the signs consistently with the forward
     * convention above). */
    const double ax = anchor_x * static_cast<double>(src_w);
    const double ay = anchor_y * static_cast<double>(src_h);

    const double inv_sx = 1.0 / scale_x;
    const double inv_sy = 1.0 / scale_y;

    /* Compose matrix components. Let cx' = cx - (translate_x + ax)
     * and cy' = cy - (translate_y + ay). Then:
     *   rx =  c * cx' + s * cy'         // rotate by -theta
     *   ry = -s * cx' + c * cy'
     *   sx_off =  rx * inv_sx           // inverse scale
     *   sy_off =  ry * inv_sy
     *   sx = sx_off + ax
     *   sy = sy_off + ay
     *
     * Flatten to [a,b,tx; c,d,ty]:
     *   sx = (c*inv_sx) * cx + (s*inv_sx) * cy + (ax + tx_term_x)
     *   sy = (-s*inv_sy) * cx + (c*inv_sy) * cy + (ay + ty_term_y)
     * where tx_term_x = -(translate_x + ax) * c * inv_sx - (translate_y + ay) * s * inv_sx
     *   and ty_term_y = (translate_x + ax) * s * inv_sy - (translate_y + ay) * c * inv_sy */
    const double tx_eff = translate_x + ax;
    const double ty_eff = translate_y + ay;

    inv.a  = static_cast<float>( c * inv_sx);
    inv.b  = static_cast<float>( s * inv_sx);
    inv.tx = static_cast<float>(ax - (tx_eff * c + ty_eff * s) * inv_sx);

    inv.c  = static_cast<float>(-s * inv_sy);
    inv.d  = static_cast<float>( c * inv_sy);
    inv.ty = static_cast<float>(ay - (-tx_eff * s + ty_eff * c) * inv_sy);

    return inv;
}

void affine_blit(uint8_t*           dst,
                 int                dst_w,
                 int                dst_h,
                 std::size_t        dst_stride_bytes,
                 const uint8_t*     src,
                 int                src_w,
                 int                src_h,
                 std::size_t        src_stride_bytes,
                 const AffineMatrix& inv) {
    for (int y = 0; y < dst_h; ++y) {
        uint8_t* dst_row = dst + static_cast<std::size_t>(y) * dst_stride_bytes;

        for (int x = 0; x < dst_w; ++x) {
            /* Inverse map: dst (x,y) → src (sx_f, sy_f). */
            const float sx_f = inv.a * static_cast<float>(x) +
                               inv.b * static_cast<float>(y) + inv.tx;
            const float sy_f = inv.c * static_cast<float>(x) +
                               inv.d * static_cast<float>(y) + inv.ty;

            /* Nearest-neighbor sample; clamp to in-bounds → else
             * transparent. lroundf for deterministic round-half-
             * away-from-zero. */
            const long isx = std::lroundf(sx_f);
            const long isy = std::lroundf(sy_f);

            uint8_t r, g, b, a;
            if (isx >= 0 && isx < src_w && isy >= 0 && isy < src_h) {
                const uint8_t* src_pixel =
                    src + static_cast<std::size_t>(isy) * src_stride_bytes +
                    static_cast<std::size_t>(isx) * 4u;
                r = src_pixel[0];
                g = src_pixel[1];
                b = src_pixel[2];
                a = src_pixel[3];
            } else {
                r = g = b = a = 0;  /* transparent */
            }

            const std::size_t off = static_cast<std::size_t>(x) * 4u;
            dst_row[off + 0] = r;
            dst_row[off + 1] = g;
            dst_row[off + 2] = b;
            dst_row[off + 3] = a;
        }
    }
}

}  // namespace me::compose
