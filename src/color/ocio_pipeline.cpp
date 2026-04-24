/*
 * me::color::OcioPipeline impl. See ocio_pipeline.hpp for scope note.
 */
#include "color/ocio_pipeline.hpp"

#include <OpenColorIO/OpenColorIO.h>

#include <cstring>
#include <stdexcept>

namespace OCIO = OCIO_NAMESPACE;

namespace me::color {

struct OcioPipeline::Impl {
    OCIO::ConstConfigRcPtr config;
};

OcioPipeline::OcioPipeline()
    : impl_(std::make_unique<Impl>()) {
    /* ACES CG is OCIO's recommended "default scene-referred with
     * Rec.709 output" config — small (ships in-tree as YAML) and
     * includes the role names `Rec.709`, `sRGB`, `lin_rec709` that
     * the forthcoming ocio-colorspace-conversions cycle will map
     * me::ColorSpace tags onto. */
    try {
        impl_->config = OCIO::Config::CreateFromBuiltinConfig(
            "cg-config-v2.1.0_aces-v1.3_ocio-v2.3");
    } catch (const OCIO::Exception& e) {
        throw std::runtime_error(std::string("OcioPipeline: failed to load builtin config: ") + e.what());
    }
    if (!impl_->config) {
        throw std::runtime_error("OcioPipeline: built-in config returned null");
    }
}

OcioPipeline::~OcioPipeline() = default;

namespace {

bool colorspace_eq(const me::ColorSpace& a, const me::ColorSpace& b) {
    return a.primaries == b.primaries
        && a.transfer  == b.transfer
        && a.matrix    == b.matrix
        && a.range     == b.range;
}

/* Map a me::ColorSpace tag to an OCIO colorspace name in the built-in
 * "cg-config-v2.1.0_aces-v1.3_ocio-v2.3" config. Returns nullptr when
 * the combination isn't in the phase-1 supported set (primaries other
 * than BT.709; PQ/HLG transfers; Matrix / Range axes carry YUV-domain
 * info that RGB OCIO processors ignore, so they don't narrow the map).
 *
 * Only three (transfer) buckets are mapped right now — matching the
 * bullet's "bt709 / sRGB / linear" ask. Other transfers are a separate
 * cycle; extending this table is the natural first step. */
const char* to_ocio_name(const me::ColorSpace& cs) {
    /* phase-1: only BT.709 primaries. sRGB primaries are actually
     * identical to Rec.709 primaries (both D65 + same chromaticity),
     * so we accept either as "BT709". Other primaries → unsupported. */
    if (cs.primaries != me::ColorSpace::Primaries::BT709 &&
        cs.primaries != me::ColorSpace::Primaries::Unspecified) {
        return nullptr;
    }

    using T = me::ColorSpace::Transfer;
    switch (cs.transfer) {
    case T::BT709:
        /* Video-encoded bt709 uses BT.1886 transfer (gamma ≈ 2.4).
         * OCIO's "Gamma 2.4 Rec.709 - Texture" is the closest built-in
         * colorspace; alias `rec709_display` resolves to the same. */
        return "Gamma 2.4 Rec.709 - Texture";
    case T::SRGB:
        /* sRGB piecewise EOTF encoding. */
        return "sRGB - Texture";
    case T::Linear:
        /* Scene-linear Rec.709 primaries. */
        return "Linear Rec.709 (sRGB)";
    case T::Unspecified:
    default:
        return nullptr;
    }
}

}  // namespace

me_status_t OcioPipeline::apply(void*                   buffer,
                                 std::size_t            byte_count,
                                 const me::ColorSpace&  src,
                                 const me::ColorSpace&  dst,
                                 std::string*           err) {
    /* Identity fast-path. Covers the common "no conversion needed"
     * case (reencode threads per-clip source space end-to-end) and
     * byte-for-byte matches IdentityPipeline — no OCIO processor
     * built, no bytes touched. */
    if (colorspace_eq(src, dst)) {
        return ME_OK;
    }

    /* Map both tags to OCIO colorspace names. nullptr means the axis
     * combination isn't in the phase-1 supported set (e.g. BT2020
     * primaries, PQ/HLG transfer). Report the side that failed. */
    const char* src_name = to_ocio_name(src);
    const char* dst_name = to_ocio_name(dst);
    if (!src_name || !dst_name) {
        if (err) {
            *err = "OcioPipeline: no OCIO role mapping for ";
            *err += src_name ? "dst" : "src";
            *err += " colorspace (phase-1 supports primaries=bt709 "
                    "and transfer in {bt709, srgb, linear})";
        }
        /* LEGIT: phase-1 color-space support is bounded to a known-good
         * subset; callers outside it receive a parseable error string. */
        return ME_E_UNSUPPORTED;
    }

    if (!buffer || byte_count == 0) {
        if (err) *err = "OcioPipeline: null or empty buffer";
        return ME_E_INVALID_ARG;
    }
    if (byte_count % 4 != 0) {
        if (err) *err = "OcioPipeline: byte_count (" + std::to_string(byte_count) +
                        ") not a multiple of 4 (RGBA8)";
        return ME_E_INVALID_ARG;
    }

    try {
        OCIO::ConstProcessorRcPtr proc =
            impl_->config->getProcessor(src_name, dst_name);
        /* Finalize for uint8 in-out — `getDefaultCPUProcessor()`
         * assumes float32 input and throws "bit-depth mismatch"
         * when PackedImageDesc declares BIT_DEPTH_UINT8.
         * OPTIMIZATION_DEFAULT lets OCIO fuse passes for the
         * uint8 path (LUT-based approximation within the integer
         * precision envelope). */
        OCIO::ConstCPUProcessorRcPtr cpu =
            proc->getOptimizedCPUProcessor(OCIO::BIT_DEPTH_UINT8,
                                            OCIO::BIT_DEPTH_UINT8,
                                            OCIO::OPTIMIZATION_DEFAULT);

        /* 1D strip layout: num_pixels × 1 × RGBA8. OCIO operates per-
         * pixel and is indifferent to 2D layout, so flattening is
         * safe — the caller's 2D image ends up with the same byte
         * sequence it started with, independently transformed pixel
         * by pixel. */
        const long num_pixels = static_cast<long>(byte_count / 4);
        OCIO::PackedImageDesc desc(
            buffer,
            num_pixels,
            /*height=*/1,
            /*numChannels=*/4,
            OCIO::BIT_DEPTH_UINT8,
            /*chanStrideBytes=*/1,
            /*xStrideBytes=*/4,
            /*yStrideBytes=*/4 * num_pixels);
        cpu->apply(desc);
        return ME_OK;
    } catch (const OCIO::Exception& e) {
        if (err) {
            *err = "OcioPipeline: OCIO error applying ";
            *err += src_name; *err += " → "; *err += dst_name;
            *err += ": "; *err += e.what();
        }
        return ME_E_INTERNAL;
    }
}

}  // namespace me::color
