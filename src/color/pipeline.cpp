/*
 * me::color::make_pipeline implementation. Split out of pipeline.hpp
 * so the ME_HAS_OCIO branch can include ocio_pipeline.hpp (which
 * depends on the Pipeline class body above) without a header cycle.
 */
#include "color/pipeline.hpp"

#if ME_HAS_OCIO
#include "color/ocio_pipeline.hpp"
#endif

namespace me::color {

std::unique_ptr<Pipeline> make_pipeline() {
#if ME_HAS_OCIO
    return std::make_unique<OcioPipeline>();
#else
    return std::make_unique<IdentityPipeline>();
#endif
}

}  // namespace me::color
