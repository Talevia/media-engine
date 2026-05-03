/* openfx_headers_smoke — compile-only smoke check that the
 * OFX 1.4 SDK headers are reachable when ME_WITH_OFX=ON.
 *
 * The TU just includes the canonical OFX headers and declares
 * a no-op function to prevent the compiler from optimizing
 * the includes away. When ME_HAS_OFX is NOT defined (the
 * default OFF build), the TU is a no-op stub — no OFX headers
 * referenced.
 *
 * Why a smoke TU and not a unit test. The test suite would
 * need ME_BUILD_TESTS + ME_WITH_OFX both ON, which is two
 * configure flags. A compile-only smoke is reachable from any
 * ME_WITH_OFX=ON build without requiring tests.
 *
 * Once `m13-openfx-plugin-cache-impl` lands its real TUs
 * (plugin_cache.cpp etc.), this smoke file can be deleted —
 * the real TUs serve as the same compile-coverage tripwire.
 */

#ifdef ME_HAS_OFX

extern "C" {
#include <ofxCore.h>
#include <ofxImageEffect.h>
#include <ofxParam.h>
#include <ofxProperty.h>
}

namespace me::openfx {

/* Reference a few OFX-defined types so the includes can't be
 * silently DCE'd. Returns the size of `OfxStatus` (always 4
 * bytes — `int` per OFX spec); the value matters less than
 * the linker-visible reference. */
unsigned long ofx_headers_smoke() {
    OfxStatus s = kOfxStatOK;
    return static_cast<unsigned long>(sizeof(OfxStatus)) +
           static_cast<unsigned long>(s);
}

}  // namespace me::openfx

#else  /* !ME_HAS_OFX */

/* Compile-only stub when OFX support is disabled. Keeps the
 * source tree balanced (file always compiles) without
 * requiring conditional add_library() machinery. */

namespace me::openfx {

unsigned long ofx_headers_smoke() {
    return 0;
}

}  // namespace me::openfx

#endif
