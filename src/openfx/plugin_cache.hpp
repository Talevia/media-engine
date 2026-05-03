/* PluginCache — load + filter OFX plugins from a list of
 * loadable paths.
 *
 * M13 OpenFX host integration — first impl piece per
 * docs/OPENFX.md "What's NOT done yet". Sibling pieces:
 *   image_effect_suite.{hpp,cpp}   — host suite tables (next)
 *   render_dispatch.{hpp,cpp}      — kOfxImageEffectActionRender
 *
 * This header + impl are gated on `ME_HAS_OFX=1` (the
 * `ME_WITH_OFX=ON` CMake build path). When ME_HAS_OFX is not
 * defined, a forward-declared API is exposed with a
 * fail-with-named-status return so source-level callers stay
 * compilable without the OFX SDK fetched.
 *
 * Design.
 *
 *   Caller supplies:
 *     - `loadable_paths`: list of `.dylib` / `.so` / `.dll`
 *       filesystem paths to try loading. Bundle (.ofx
 *       directory) navigation is a follow-up; first cycle
 *       expects direct paths to the plugin binary.
 *     - `allowlist`: set of plugin identifier strings
 *       (`OfxPlugin::pluginIdentifier`) the host is willing
 *       to load. Plugins NOT in the set are discovered but
 *       not returned — the host's license-whitelist gate.
 *       An empty allowlist rejects everything (deny-by-default).
 *
 *   PluginCache::discover():
 *     1. For each path in loadable_paths:
 *          a. dlopen() the file. On failure, append a
 *             diagnostic and continue (one bad plugin
 *             doesn't kill the whole cache).
 *          b. dlsym() OfxGetNumberOfPlugins +
 *             OfxGetPlugin. Missing → log + dlclose + skip.
 *          c. Iterate plugins via OfxGetPlugin(i):
 *               - Read pluginIdentifier from the OfxPlugin*.
 *               - If pluginIdentifier ∈ allowlist:
 *                   record (handle, OfxPlugin*, identifier,
 *                   source path) into the cache's own
 *                   vector.
 *               - Else: log "plugin X rejected
 *                   (not in allowlist)" + don't keep.
 *     2. Return a span of accepted entries.
 *
 *   PluginCache lifetime:
 *     - Owns the dlopen handles. Destructor dlclose()s all.
 *     - All returned OfxPlugin* pointers are invalidated
 *       when the cache is destroyed; consumers must not
 *       outlive the cache.
 *     - Move-only (cache holds raw pointers + handles that
 *       can't be copied).
 *
 * Argument-shape rejects:
 *   - loadable_paths null / out null    → ME_E_INVALID_ARG
 *
 * Per-path failures are diagnostics, not aborts:
 *   - dlopen failure                     → diag + skip
 *   - missing entry-point symbols        → diag + skip
 *   - plugin not in allowlist            → diag + skip
 *
 * Entry-point return values:
 *   - ME_OK                              — at least 0
 *                                          plugins discovered
 *                                          (empty cache is valid)
 *   - ME_E_INVALID_ARG                   — null arg
 *   - ME_E_UNSUPPORTED                   — built without
 *                                          ME_WITH_OFX (no OFX
 *                                          headers available
 *                                          to compile against)
 */
#pragma once

#include "media_engine/types.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef ME_HAS_OFX
struct OfxPlugin;  /* opaque to non-OFX TUs */
#endif

namespace me::openfx {

#ifdef ME_HAS_OFX

struct PluginInfo {
    /* Borrowed pointer; valid only while the owning
     * PluginCache is alive. */
    OfxPlugin*  plugin = nullptr;
    /* Plugin identifier (from OfxPlugin::pluginIdentifier).
     * Stable across loads of the same plugin. */
    std::string identifier;
    /* Filesystem path the plugin was loaded from. */
    std::string source_path;
};

class PluginCache {
public:
    PluginCache();
    ~PluginCache();

    /* Move-only — cache owns dlopen handles. */
    PluginCache(const PluginCache&)            = delete;
    PluginCache& operator=(const PluginCache&) = delete;
    PluginCache(PluginCache&&)            noexcept;
    PluginCache& operator=(PluginCache&&) noexcept;

    /* Discover + filter. See header doc. */
    me_status_t discover(
        const std::vector<std::string>&        loadable_paths,
        const std::unordered_set<std::string>& allowlist,
        std::string*                           err);

    /* Accepted plugins. Empty until discover() is called. */
    const std::vector<PluginInfo>& plugins() const noexcept;

    /* Diagnostics from the most recent discover() call. Each
     * entry corresponds to a per-path failure or a per-plugin
     * rejection (not in allowlist). Reset by each discover(). */
    const std::vector<std::string>& diagnostics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#else  /* !ME_HAS_OFX */

/* Stub when built without ME_WITH_OFX. Calling discover()
 * returns ME_E_UNSUPPORTED with a named diag. */
class PluginCache {
public:
    PluginCache() = default;
    ~PluginCache() = default;

    me_status_t discover(
        const std::vector<std::string>&        /*loadable_paths*/,
        const std::unordered_set<std::string>& /*allowlist*/,
        std::string*                           err);

    /* Stubs return empty vectors. */
    const std::vector<int>& plugins() const noexcept;
    const std::vector<std::string>& diagnostics() const noexcept;
};

#endif

}  // namespace me::openfx
