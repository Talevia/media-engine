/*
 * me::color::OcioPipeline — OpenColorIO-backed Pipeline implementation.
 *
 * Lands with the `ocio-pipeline-enable` cycle: CMake now pulls
 * OpenColorIO v2.5.1 via FetchContent (unblocked upstream fix passes
 * CMAKE_POLICY_VERSION_MINIMUM into its nested yaml-cpp_install
 * ExternalProject) and `ME_HAS_OCIO` is defined on the media_engine
 * target so this header compiles.
 *
 * Current scope is skeletal: the constructor wires up an OCIO
 * built-in config (ACES CG) so the library link path is exercised.
 * `apply()` fast-paths identity transforms (src == dst) as ME_OK and
 * returns ME_E_UNSUPPORTED for non-identity pairs with a diagnostic
 * pointing to the `ocio-colorspace-conversions` backlog item that
 * will fill in the bt709 ↔ sRGB ↔ linear math.
 *
 * Keeping the scope small deliberately — flipping ME_WITH_OCIO default
 * on already obligates every fresh build to ~5 minutes of OCIO
 * compilation; the colorspace matrix work is a separable cycle of its
 * own with its own numerical test bed.
 */
#pragma once

#include "color/pipeline.hpp"

#include <memory>
#include <string>

namespace OpenColorIO_v2_5 { class Config; }
namespace OpenColorIO = OpenColorIO_v2_5;

namespace me::color {

class OcioPipeline final : public Pipeline {
public:
    /* Creates an OcioPipeline wrapping OCIO's built-in ACES CG
     * config. Throws std::runtime_error if OCIO rejects the config
     * identifier — not expected in practice (OCIO ships the config
     * in-tree). Construction cost is small (~ms); callers can
     * stand one up per engine. */
    OcioPipeline();
    ~OcioPipeline() override;

    me_status_t apply(void*                   buffer,
                       std::size_t            byte_count,
                       const me::ColorSpace&  src,
                       const me::ColorSpace&  dst,
                       std::string*           err) override;

private:
    /* OCIO config pointer is held via std::shared_ptr internally; the
     * class holds it as opaque `void*` in the header so consumers of
     * ocio_pipeline.hpp don't need OCIO headers in their translation
     * unit. The concrete pointer type is reconstituted in the .cpp. */
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace me::color
