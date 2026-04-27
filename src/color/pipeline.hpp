/*
 * me::color::Pipeline — reserved abstraction for color-managed compositing.
 *
 * Purpose of this file (and of the `src/color/` subdir) is to **reserve
 * the namespace** ahead of the real OpenColorIO integration. A concrete
 * `OcioPipeline` subclass lands in the same subdir once the OCIO
 * FetchContent path is actually exercised (the CMake side is gated
 * behind `ME_WITH_OCIO` — off by default). Until then, every caller
 * that wants "color pipeline" semantics lives with `IdentityPipeline`,
 * a no-op transform that documents the intended API shape.
 *
 * `apply` takes a `me::ColorSpace` pair (already parsed by
 * `asset-colorspace-field` into typed-enum form — not strings). Buffer
 * handle is a raw `void*` + byte count for now; replace with a typed
 * `me::frame::Pixmap` when M2 compose brings one. Header-only on
 * purpose — no .cpp, no linker dep until OCIO is wired. `-Werror` clean.
 */
#pragma once

#include "media_engine/types.h"
#include "timeline/timeline_impl.hpp"     /* me::ColorSpace */

#include <cstddef>
#include <memory>
#include <string>

namespace me::color {

/* Abstract policy: every concrete pipeline implements `apply` over an
 * RGB / YUV byte buffer. `dst` / `src` carry the typed tags; pipelines
 * that need a GPU context, LUT cache, or working-space promotion
 * construct it lazily so the interface stays small.
 *
 * Output: in-place; implementations must be byte-deterministic given
 * same input + tags (VISION §3.3 / §5.3). */
class Pipeline {
public:
    virtual ~Pipeline() = default;

    /* Returns ME_OK on success. ME_E_UNSUPPORTED when the concrete
     * pipeline can't honour the source / target pair (e.g. PQ → HLG on a
     * linear-only backend). Diagnostic string written to *err. */
    virtual me_status_t apply(void*                   buffer,
                               std::size_t            byte_count,
                               const me::ColorSpace&  src,
                               const me::ColorSpace&  dst,
                               std::string*           err) = 0;
};

/* Identity: no-op pass-through. Used by every caller that hasn't been
 * migrated to a real color pipeline yet. Keeps callsites shape-stable
 * so the real OCIO wiring is a typed-pointer swap, not an API churn. */
class IdentityPipeline final : public Pipeline {
public:
    me_status_t apply(void*                   /*buffer*/,
                       std::size_t            /*byte_count*/,
                       const me::ColorSpace&  /*src*/,
                       const me::ColorSpace&  /*dst*/,
                       std::string*           /*err*/) override {
        return ME_OK;
    }
};

/* Factory: hand out the canonical Pipeline for this build. Callers
 * that will eventually need color management (compose, frame-server,
 * thumbnailer) use this factory instead of `new IdentityPipeline` so
 * the day OcioPipeline lands the switch is one compile-def flip
 * (`ME_HAS_OCIO` — set by `src/CMakeLists.txt` when `ME_WITH_OCIO=ON`).
 *
 * Definition lives in `src/color/pipeline.cpp` so that the
 * ME_HAS_OCIO branch can include `color/ocio_pipeline.hpp` (which
 * itself depends on the Pipeline class body in this header — an
 * `#include` cycle from the factory definition back to ocio_pipeline
 * is avoided by moving the body out-of-line).
 *
 * ME_HAS_OCIO branch returns an `OcioPipeline`; otherwise
 * `IdentityPipeline`.
 *
 * `config_path` is forwarded to `OcioPipeline`'s constructor on the
 * ME_HAS_OCIO branch — see `ocio_pipeline.hpp` for the resolution
 * order (explicit path → `$OCIO` env var → built-in). NULL or empty
 * = follow env / builtin. Ignored on the IdentityPipeline branch. */
std::unique_ptr<Pipeline> make_pipeline(const char* config_path = nullptr);

}  // namespace me::color
