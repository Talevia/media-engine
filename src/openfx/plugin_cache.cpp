/* PluginCache impl. See header. */
#include "openfx/plugin_cache.hpp"

#ifdef ME_HAS_OFX

extern "C" {
#include <ofxCore.h>
#include <ofxImageEffect.h>
}

#include <dlfcn.h>

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace me::openfx {

namespace {

/* OFX plugin entry-point function signatures. The host
 * resolves these via dlsym after dlopen'ing the bundle. */
using OfxGetNumberOfPluginsFn = int (*)(void);
using OfxGetPluginFn          = OfxPlugin* (*)(int);

}  // namespace

struct PluginCache::Impl {
    /* Per-loaded-bundle entry. Keeps dlopen handle alive so
     * OfxPlugin* pointers stay valid. */
    struct Loaded {
        void*       handle = nullptr;
        std::string path;
    };
    std::vector<Loaded>      loaded_bundles;
    std::vector<PluginInfo>  accepted_plugins;
    std::vector<std::string> diagnostics;

    ~Impl() {
        /* Plugin pointers belong to the loaded library; close
         * after we've nulled the accepted_plugins vector so
         * any consumer iterating doesn't see dangling pointers
         * (defensive — consumers shouldn't outlive the cache,
         * but dtor order makes this safer). */
        accepted_plugins.clear();
        for (auto& l : loaded_bundles) {
            if (l.handle) dlclose(l.handle);
        }
    }
};

PluginCache::PluginCache()
    : impl_(std::make_unique<Impl>()) {}

PluginCache::~PluginCache() = default;

PluginCache::PluginCache(PluginCache&&) noexcept            = default;
PluginCache& PluginCache::operator=(PluginCache&&) noexcept = default;

me_status_t PluginCache::discover(
    const std::vector<std::string>&        loadable_paths,
    const std::unordered_set<std::string>& allowlist,
    std::string*                           err) {
    if (!impl_) return ME_E_INVALID_ARG;

    impl_->accepted_plugins.clear();
    impl_->diagnostics.clear();
    /* Don't drop already-loaded bundles — discover() can be
     * called multiple times to accumulate plugins from
     * different directories. New paths are dlopen'd alongside.
     * The dtor handles eventual cleanup. */

    for (const auto& path : loadable_paths) {
        void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (!handle) {
            const char* dlerr = dlerror();
            impl_->diagnostics.push_back(
                "PluginCache: dlopen('" + path + "') failed: " +
                (dlerr ? dlerr : "unknown"));
            continue;
        }

        auto get_num = reinterpret_cast<OfxGetNumberOfPluginsFn>(
            dlsym(handle, "OfxGetNumberOfPlugins"));
        auto get_one = reinterpret_cast<OfxGetPluginFn>(
            dlsym(handle, "OfxGetPlugin"));
        if (!get_num || !get_one) {
            impl_->diagnostics.push_back(
                "PluginCache: '" + path +
                "' missing OfxGetNumberOfPlugins / OfxGetPlugin "
                "(not an OFX bundle?)");
            dlclose(handle);
            continue;
        }

        const int n = get_num();
        if (n <= 0) {
            impl_->diagnostics.push_back(
                "PluginCache: '" + path +
                "' reports zero plugins; closing");
            dlclose(handle);
            continue;
        }

        /* Track this handle; it owns the OfxPlugin* memory
         * via static storage in the loaded library. */
        impl_->loaded_bundles.push_back({handle, path});

        for (int i = 0; i < n; ++i) {
            OfxPlugin* p = get_one(i);
            if (!p) {
                impl_->diagnostics.push_back(
                    "PluginCache: OfxGetPlugin(" + std::to_string(i) +
                    ") returned NULL in '" + path + "'");
                continue;
            }
            const std::string ident =
                p->pluginIdentifier ? p->pluginIdentifier : "";
            if (ident.empty()) {
                impl_->diagnostics.push_back(
                    "PluginCache: plugin index " + std::to_string(i) +
                    " in '" + path + "' has empty pluginIdentifier");
                continue;
            }
            if (allowlist.count(ident) == 0) {
                impl_->diagnostics.push_back(
                    "PluginCache: plugin '" + ident +
                    "' rejected (not in allowlist; source: " + path + ")");
                continue;
            }

            PluginInfo info;
            info.plugin      = p;
            info.identifier  = ident;
            info.source_path = path;
            impl_->accepted_plugins.push_back(std::move(info));
        }
    }

    if (err) {
        /* Leave err empty on ME_OK; caller can still query
         * `diagnostics()` for the per-path messages. */
        err->clear();
    }
    return ME_OK;
}

const std::vector<PluginInfo>& PluginCache::plugins() const noexcept {
    return impl_->accepted_plugins;
}

const std::vector<std::string>& PluginCache::diagnostics() const noexcept {
    return impl_->diagnostics;
}

}  // namespace me::openfx

#else  /* !ME_HAS_OFX */

#include <string>
#include <unordered_set>
#include <vector>

namespace me::openfx {

me_status_t PluginCache::discover(
    const std::vector<std::string>&        /*loadable_paths*/,
    const std::unordered_set<std::string>& /*allowlist*/,
    std::string*                           err) {
    if (err) {
        *err = "PluginCache::discover: built without ME_WITH_OFX (OFX SDK "
               "headers not fetched); rebuild with -DME_WITH_OFX=ON";
    }
    return ME_E_UNSUPPORTED;
}

const std::vector<int>& PluginCache::plugins() const noexcept {
    static const std::vector<int> empty;
    return empty;
}

const std::vector<std::string>& PluginCache::diagnostics() const noexcept {
    static const std::vector<std::string> empty;
    return empty;
}

}  // namespace me::openfx

#endif
