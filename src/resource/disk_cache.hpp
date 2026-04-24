/*
 * me::resource::DiskCache — persistent RGBA8 frame cache backed
 * by a filesystem directory.
 *
 * M6 exit criterion "disk cache (cache_dir 配置生效)" foundation.
 * Engine-owned instance; consumer is the frame server
 * (Previewer::frame_at + future scrubbing cache layer).
 *
 * Design:
 *   - One file per cached frame. Name = `<hex_hash>.bin` where
 *     hash is a caller-supplied content hash (per VISION §3.3;
 *     AssetHashCache produces sha256 hex strings that work
 *     naturally as keys).
 *   - File format: fixed 16-byte little-endian header
 *     { uint32 width, uint32 height, uint32 stride, uint32
 *     reserved }, followed by stride × height bytes of RGBA8.
 *     Fixed header keeps readers simple and future-extensible
 *     (reserved field can grow into a format version if needed).
 *   - Lazy dir creation: ctor stores the path but doesn't mkdir;
 *     first `put` call creates the directory if missing. Silent
 *     degradation — any filesystem error → `put` is a no-op, so
 *     misconfigured cache_dir doesn't break the render path.
 *
 * API contract:
 *   - `put(hash, rgba, w, h, stride)`: writes the entry. Returns
 *     false on filesystem error; caller can ignore the return
 *     value — failures are purely diagnostic.
 *   - `get(hash) -> optional<CachedFrame>`: reads the entry if
 *     present. Returns nullopt on missing file / corrupt header
 *     / short read.
 *   - `invalidate(hash)`: removes one entry.
 *   - `clear()`: removes all entries this cache owns.
 *
 * Threading: each public method locks an internal mutex, so
 * concurrent readers/writers are safe. No LRU / size cap in
 * phase-1 — consumers invalidate explicitly, or the user
 * clears the cache dir manually. Sized eviction lands with
 * a follow-up bullet when scrubbing workloads surface memory
 * pressure.
 *
 * Ownership: DiskCache doesn't own the directory — multiple
 * processes / engines can share one cache_dir safely (per-file
 * atomic writes via write-to-temp + rename). Clearing via
 * `clear()` only removes files this cache wrote (tracked in
 * an in-memory set? No — phase-1 just walks the dir; users
 * should use a dedicated dir per engine instance to avoid
 * cross-contamination).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace me::resource {

struct CachedFrame {
    int                  width  = 0;
    int                  height = 0;
    int                  stride = 0;
    std::vector<uint8_t> rgba;
};

class DiskCache {
public:
    /* Construct with a directory path. Empty / null path →
     * instance is permanently disabled (put/get no-op). */
    explicit DiskCache(std::string cache_dir);

    DiskCache(const DiskCache&)            = delete;
    DiskCache& operator=(const DiskCache&) = delete;

    /* Write a frame entry. `rgba` must have at least
     * `stride × height` bytes. Returns true iff the entry was
     * persisted; false for any reason (no cache dir, mkdir
     * failed, write failed, short write). */
    bool put(const std::string& hash,
             const uint8_t*     rgba,
             int                width,
             int                height,
             int                stride);

    /* Read a cached entry. Returns nullopt if absent / corrupt. */
    std::optional<CachedFrame> get(const std::string& hash);

    /* Remove one entry. No-op if absent. */
    void invalidate(const std::string& hash);

    /* Remove all entries under cache_dir matching the ".bin"
     * extension. Doesn't touch other files a user may have put
     * there (defensive: avoid `rm -rf`-style foot-guns). */
    void clear();

    /* Path of the configured cache directory (for diagnostic
     * logging; not for caller-side I/O). */
    const std::string& dir() const noexcept { return dir_; }

    /* True iff construction succeeded with a non-empty dir. An
     * enabled cache may still fail writes on I/O errors; this
     * reports the configuration-time gate, not runtime status. */
    bool enabled() const noexcept { return !dir_.empty(); }

private:
    std::string   dir_;   /* empty = disabled */
    std::mutex    mu_;    /* guards concurrent put/get/invalidate */
};

}  // namespace me::resource
