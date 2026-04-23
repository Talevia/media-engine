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
}  // namespace

me_status_t OcioPipeline::apply(void*                   /*buffer*/,
                                 std::size_t            /*byte_count*/,
                                 const me::ColorSpace&  src,
                                 const me::ColorSpace&  dst,
                                 std::string*           err) {
    /* Identity fast-path. This covers every current consumer (reencode
     * path threads per-clip source space all the way through, so when
     * nothing's being converted, src == dst); no OCIO processor is
     * built and no bytes are touched. Matches IdentityPipeline's
     * contract byte-for-byte. */
    if (colorspace_eq(src, dst)) {
        return ME_OK;
    }

    /* Non-identity conversions are the scope of the follow-up
     * ocio-colorspace-conversions backlog bullet. Return UNSUPPORTED
     * with a message that names the concrete axes that differ so the
     * implementer can scope the first pass (e.g. "only transfer differs
     * → just a gamma curve"). */
    if (err) {
        *err = "OcioPipeline: non-identity colorspace conversion not yet "
               "implemented (see ocio-colorspace-conversions backlog item); "
               "differing axes:";
        if (src.primaries != dst.primaries) *err += " primaries";
        if (src.transfer  != dst.transfer)  *err += " transfer";
        if (src.matrix    != dst.matrix)    *err += " matrix";
        if (src.range     != dst.range)     *err += " range";
    }
    return ME_E_UNSUPPORTED;
}

}  // namespace me::color
