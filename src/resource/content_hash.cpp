#include "resource/content_hash.hpp"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace me::resource {

namespace {

std::string strip_file_scheme(std::string_view p) {
    constexpr std::string_view scheme{"file://"};
    if (p.size() >= scheme.size() &&
        p.compare(0, scheme.size(), scheme) == 0) {
        p.remove_prefix(scheme.size());
    }
    return std::string{p};
}

struct ShaDel {
    void operator()(AVSHA* p) const { av_freep(&p); }
};
using ShaPtr = std::unique_ptr<AVSHA, ShaDel>;

std::string digest_to_hex(const uint8_t (&digest)[32]) {
    static const char* kHex = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; ++i) {
        out[i * 2]     = kHex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[digest[i] & 0xF];
    }
    return out;
}

}  // namespace

std::string sha256_hex(const uint8_t* data, size_t len) {
    ShaPtr ctx(av_sha_alloc());
    if (!ctx) return {};
    av_sha_init(ctx.get(), 256);
    if (data && len) av_sha_update(ctx.get(), data, len);
    uint8_t digest[32];
    av_sha_final(ctx.get(), digest);
    return digest_to_hex(digest);
}

std::string sha256_hex_streaming(std::string_view path_or_uri, std::string* err) {
    const std::string path = strip_file_scheme(path_or_uri);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "open " + path + ": " + std::strerror(errno);
        return {};
    }
    ShaPtr ctx(av_sha_alloc());
    if (!ctx) {
        std::fclose(f);
        if (err) *err = "av_sha_alloc failed";
        return {};
    }
    av_sha_init(ctx.get(), 256);

    uint8_t buf[64 * 1024];
    while (true) {
        const size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        av_sha_update(ctx.get(), buf, n);
    }
    const bool had_err = std::ferror(f) != 0;
    std::fclose(f);
    if (had_err) {
        if (err) *err = "read " + path + ": " + std::strerror(errno);
        return {};
    }
    uint8_t digest[32];
    av_sha_final(ctx.get(), digest);
    return digest_to_hex(digest);
}

}  // namespace me::resource
