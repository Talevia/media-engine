#include "resource/disk_cache.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace me::resource {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kHeaderBytes = 16;

std::string entry_path(const std::string& dir, const std::string& hash) {
    return dir + "/" + hash + ".bin";
}

/* Write a 4-byte little-endian uint32 into the output buffer. */
void write_u32_le(uint8_t* dst, std::uint32_t v) {
    dst[0] = static_cast<uint8_t>(v        & 0xFF);
    dst[1] = static_cast<uint8_t>((v >>  8) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

std::uint32_t read_u32_le(const uint8_t* src) {
    return (static_cast<std::uint32_t>(src[0])      ) |
           (static_cast<std::uint32_t>(src[1]) <<  8) |
           (static_cast<std::uint32_t>(src[2]) << 16) |
           (static_cast<std::uint32_t>(src[3]) << 24);
}

}  // namespace

DiskCache::DiskCache(std::string cache_dir)
    : dir_(std::move(cache_dir)) {}

bool DiskCache::put(const std::string& hash,
                     const uint8_t*     rgba,
                     int                width,
                     int                height,
                     int                stride) {
    if (dir_.empty() || hash.empty() || !rgba) return false;
    if (width <= 0 || height <= 0 || stride <= 0) return false;

    std::lock_guard<std::mutex> lk(mu_);

    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) return false;  /* mkdir failed (permissions, full disk, …) */

    /* Write to a temp name + rename, so concurrent readers never
     * see a partial file. rename() is atomic on same-fs POSIX. */
    const std::string final_path = entry_path(dir_, hash);
    const std::string tmp_path   = final_path + ".tmp";

    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    /* Header. */
    uint8_t header[kHeaderBytes] = {0};
    write_u32_le(header + 0,  static_cast<std::uint32_t>(width));
    write_u32_le(header + 4,  static_cast<std::uint32_t>(height));
    write_u32_le(header + 8,  static_cast<std::uint32_t>(stride));
    /* header[12..15] reserved = 0 */
    out.write(reinterpret_cast<const char*>(header), kHeaderBytes);
    if (!out) { fs::remove(tmp_path, ec); return false; }

    /* Body: row-major stride * height bytes. */
    const std::size_t body_bytes =
        static_cast<std::size_t>(stride) * static_cast<std::size_t>(height);
    out.write(reinterpret_cast<const char*>(rgba),
              static_cast<std::streamsize>(body_bytes));
    if (!out) { fs::remove(tmp_path, ec); return false; }
    out.close();
    if (!out) { fs::remove(tmp_path, ec); return false; }

    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        std::error_code remove_ec;
        fs::remove(tmp_path, remove_ec);
        return false;
    }
    return true;
}

std::optional<CachedFrame> DiskCache::get(const std::string& hash) {
    if (dir_.empty() || hash.empty()) return std::nullopt;

    std::lock_guard<std::mutex> lk(mu_);

    const std::string path = entry_path(dir_, hash);
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    uint8_t header[kHeaderBytes] = {0};
    in.read(reinterpret_cast<char*>(header), kHeaderBytes);
    if (!in) return std::nullopt;
    if (in.gcount() != static_cast<std::streamsize>(kHeaderBytes)) {
        return std::nullopt;  /* file too small; corrupted */
    }

    CachedFrame out;
    out.width  = static_cast<int>(read_u32_le(header + 0));
    out.height = static_cast<int>(read_u32_le(header + 4));
    out.stride = static_cast<int>(read_u32_le(header + 8));

    /* Sanity-check to avoid gigantic allocations on corrupt files.
     * 16k × 16k RGBA is ~1 GB — far above any sane cached frame. */
    if (out.width <= 0 || out.width > 16384 ||
        out.height <= 0 || out.height > 16384 ||
        out.stride <= 0 || out.stride > 16384 * 8) {
        return std::nullopt;
    }

    const std::size_t body_bytes =
        static_cast<std::size_t>(out.stride) *
        static_cast<std::size_t>(out.height);
    out.rgba.resize(body_bytes);
    in.read(reinterpret_cast<char*>(out.rgba.data()),
            static_cast<std::streamsize>(body_bytes));
    if (!in) return std::nullopt;
    if (in.gcount() != static_cast<std::streamsize>(body_bytes)) {
        return std::nullopt;
    }
    return out;
}

void DiskCache::invalidate(const std::string& hash) {
    if (dir_.empty() || hash.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    std::error_code ec;
    fs::remove(entry_path(dir_, hash), ec);
}

void DiskCache::clear() {
    if (dir_.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    std::error_code ec;
    /* Iterate the directory, remove `.bin` files only. */
    fs::directory_iterator it(dir_, ec);
    if (ec) return;
    for (const auto& entry : it) {
        std::error_code remove_ec;
        if (entry.is_regular_file(remove_ec) &&
            entry.path().extension() == ".bin") {
            fs::remove(entry.path(), remove_ec);
        }
    }
}

}  // namespace me::resource
