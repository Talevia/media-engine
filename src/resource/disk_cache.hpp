/*
 * me::resource::DiskCache — persistent RGBA8 frame cache backed
 * by a filesystem directory.
 *
 * M6 exit criterion "disk cache (cache_dir 配置生效)" foundation.
 * Engine-owned instance; consumer is the frame server
 * (me_render_frame + future scrubbing cache layer).
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

#include <atomic>
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
    /* Construct with a directory path and optional size cap.
     * Empty / null path → instance is permanently disabled
     * (put/get no-op). `limit_bytes == 0` disables eviction
     * (unlimited cache, original phase-1 behaviour). When
     * positive, the ctor scans `cache_dir` for existing `.bin`
     * entries and initialises the running `disk_bytes_` total;
     * subsequent `put`s evict oldest-by-mtime entries if a new
     * write would push the total over the cap. */
    explicit DiskCache(std::string cache_dir, int64_t limit_bytes = 0);

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

    /* Remove every entry whose hash starts with `prefix`. Useful
     * when keys follow an `<asset_hash>:<suffix>` convention and
     * an asset-level invalidation should cascade. Returns the
     * count of entries removed. O(entries) scan; disabled cache
     * returns 0. */
    std::size_t invalidate_by_prefix(const std::string& prefix);

    /* Cumulative hit/miss counters — get() increments one per
     * call. Survives clear(); reset via reset_counters(). */
    std::int64_t hit_count()  const noexcept { return hits_.load();   }
    std::int64_t miss_count() const noexcept { return misses_.load(); }
    void         reset_counters() noexcept {
        hits_.store(0);
        misses_.store(0);
    }

    /* Path of the configured cache directory (for diagnostic
     * logging; not for caller-side I/O). */
    const std::string& dir() const noexcept { return dir_; }

    /* True iff construction succeeded with a non-empty dir. An
     * enabled cache may still fail writes on I/O errors; this
     * reports the configuration-time gate, not runtime status. */
    bool enabled() const noexcept { return !dir_.empty(); }

    /* Current disk footprint in bytes (sum of `.bin` file sizes
     * managed by this instance). Reads the running counter — no
     * filesystem scan. Matches `me_cache_stats.disk_bytes_used`
     * semantics. */
    std::int64_t disk_bytes_used() const noexcept { return disk_bytes_.load(); }

    /* Configured size limit. 0 = unlimited. */
    std::int64_t disk_bytes_limit() const noexcept { return limit_bytes_; }

private:
    /* Evict the single oldest-by-mtime `.bin` entry that isn't
     * named `<skip_hash>.bin`. Returns true iff an entry was
     * removed (updates `disk_bytes_` accordingly). Must be called
     * with `mu_` held. */
    bool evict_one_oldest(const std::string& skip_hash);

    std::string                dir_;           /* empty = disabled */
    std::int64_t               limit_bytes_;   /* 0 = unlimited */
    std::mutex                 mu_;            /* guards put/get/invalidate */
    std::atomic<std::int64_t>  hits_{0};
    std::atomic<std::int64_t>  misses_{0};
    std::atomic<std::int64_t>  disk_bytes_{0};  /* running total of .bin bytes */
};

}  // namespace me::resource
