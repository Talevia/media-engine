/*
 * 12_cache_invalidate — host-driven cache invalidation demo.
 *
 * Why this example exists. The C API exposes
 * `me_cache_invalidate_asset(eng, content_hash)` (cache.h:40), but
 * `grep -rn 'me_cache_invalidate_asset' examples/` was empty before
 * this cycle — hosts asking "when do I call it?" / "what string
 * goes in?" had no canonical answer in the public examples tree.
 *
 * What this program actually demonstrates:
 *
 *   1. The host pre-supplies a deterministic `contentHash` on the
 *      asset (recommended per TIMELINE_SCHEMA.md §Asset). Real
 *      hosts derive it from `sha256(file_bytes)` — engine-side
 *      it's an opaque cache key, not a checksum the engine
 *      re-validates against file bytes. We use a fixed pseudo-
 *      hash here so the demo stays deterministic without shelling
 *      out to shasum.
 *
 *   2. First `me_render_frame` decodes the source and populates
 *      both AssetHashCache (URI → contentHash mapping, in-memory)
 *      and DiskCache (frame bytes, on disk). `miss_count` rises.
 *
 *   3. Second `me_render_frame` for the same time finds the entry
 *      → cache hit. `hit_count` rises.
 *
 *   4. `me_cache_invalidate_asset(eng, "sha256:<hash>")` drops both
 *      the AssetHashCache entry AND every DiskCache entry derived
 *      from this asset. DiskCache reaches the graph-hash-keyed
 *      entries via the asset_hash → set<cache_key> side-index
 *      that `me_render_frame`'s put populates with the hashes of
 *      every asset URI flowing through the compiled graph's
 *      IoDemux nodes. The third render therefore misses and re-
 *      decodes; both `entry_count` and `disk_bytes_used` drop
 *      across the call.
 *
 *   5. `me_cache_clear` remains the universal escape hatch — it
 *      drops every entry regardless of side-index state (e.g.
 *      after a process restart that empties the in-memory index
 *      while leaving on-disk `.bin` files intact). The 4th
 *      render below demonstrates the same.
 *
 * Usage:
 *   12_cache_invalidate <source.mp4>
 *
 * Exits 0 on success, 1 on engine errors. Prints stats labeled
 * before / after-1st-render-miss / after-2nd-render-hit /
 * after-invalidate-asset / after-3rd-render / after-cache-clear /
 * after-4th-render-miss to stderr.
 *
 * Verification contract:
 *
 *   - `entry_count` (AssetHashCache size) drops by ≥ 1 across
 *     `me_cache_invalidate_asset`.
 *   - `miss_count` climbs by ≥ 1 across the 3rd render (DiskCache
 *     side-index drops the derived entry).
 *   - `miss_count` climbs by ≥ 1 across `me_cache_clear` +
 *     subsequent render.
 */
#include <media_engine.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void print_stats(const char* label, const me_cache_stats_t* s) {
    fprintf(stderr,
            "  [%s] hits=%lld misses=%lld entries=%lld disk_used=%lld\n",
            label,
            (long long)s->hit_count,  (long long)s->miss_count,
            (long long)s->entry_count,
            (long long)s->disk_bytes_used);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <source.mp4>\n", argv[0]);
        return 2;
    }
    const char* source = argv[1];

    /* Disk cache must be on for the cache-clear path to evict
     * frame entries; AssetHashCache is in-memory and always live. */
    const char* cache_dir = "/tmp/me_cache_invalidate_demo";
    mkdir(cache_dir, 0755);
    /* Demo flow needs a fresh cache dir: pre-existing .bin files from
     * a prior run would make the 1st render a hit, which skips the
     * put-with-assets call that populates the side-index. Then the
     * invalidate-asset cascade has nothing to evict and the
     * verification fails spuriously. Hosts in production typically
     * keep cache_dir warm — that's a feature, not a bug — so this
     * cleanup is demo-specific. */
    DIR* d = opendir(cache_dir);
    if (d) {
        struct dirent* e;
        char path[1024];
        while ((e = readdir(d))) {
            const size_t len = strlen(e->d_name);
            if (len < 4) continue;
            if (strcmp(e->d_name + len - 4, ".bin") != 0) continue;
            snprintf(path, sizeof path, "%s/%s", cache_dir, e->d_name);
            unlink(path);
        }
        closedir(d);
    }

    me_engine_config_t cfg = {0};
    cfg.cache_dir = cache_dir;

    me_engine_t* eng = NULL;
    me_status_t s = me_engine_create(&cfg, &eng);
    if (s != ME_OK) {
        fprintf(stderr, "engine_create: %s\n", me_status_str(s));
        return 1;
    }

    /* The contentHash is host-chosen and used both as the asset
     * cache key in the timeline JSON AND as the argument to
     * me_cache_invalidate_asset. Real hosts derive it from
     * sha256(file). */
    static const char kAssetHash[] =
        "sha256:000000000000000000000000000000000000000000000000000000000000beef";

    char uri[1024];
    snprintf(uri, sizeof uri, "file://%s", source);

    char json[2048];
    snprintf(json, sizeof json,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"frameRate\":  {\"num\":25,\"den\":1},\n"
        "  \"resolution\": {\"width\":640,\"height\":480},\n"
        "  \"colorSpace\": {\"primaries\":\"bt709\",\"transfer\":\"bt709\","
                          "\"matrix\":\"bt709\",\"range\":\"limited\"},\n"
        "  \"assets\": [{\"id\":\"a1\",\"uri\":\"%s\",\"contentHash\":\"%s\"}],\n"
        "  \"compositions\": [{\n"
        "    \"id\":\"main\",\n"
        "    \"duration\":{\"num\":1,\"den\":1},\n"
        "    \"tracks\":[{\n"
        "      \"id\":\"v0\",\"kind\":\"video\",\"clips\":[\n"
        "        {\"type\":\"video\",\"id\":\"c0\",\"assetId\":\"a1\",\n"
        "         \"timeRange\":{\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":1,\"den\":1}},\n"
        "         \"sourceRange\":{\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":1,\"den\":1}}}\n"
        "      ]}]\n"
        "  }],\n"
        "  \"output\": {\"compositionId\":\"main\"}\n"
        "}\n", uri, kAssetHash);

    me_timeline_t* tl = NULL;
    s = me_timeline_load_json(eng, json, strlen(json), &tl);
    if (s != ME_OK) {
        fprintf(stderr, "load_json: %s (%s)\n",
                me_status_str(s), me_engine_last_error(eng));
        me_engine_destroy(eng);
        return 1;
    }

    const me_rational_t t = {0, 1};
    int rc = 0;
    int local_failure = 0;

    /* Macro-light helper: render at `t`, destroy frame, fail loud. */
    #define RENDER_OR_FAIL(label) do {                                    \
        me_frame_t* _f = NULL;                                            \
        const me_status_t _s = me_render_frame(eng, tl, t, &_f);          \
        if (_s != ME_OK) {                                                \
            fprintf(stderr, "render_frame[%s]: %s (%s)\n",                \
                    label, me_status_str(_s), me_engine_last_error(eng)); \
            local_failure = 1;                                            \
        } else {                                                          \
            me_frame_destroy(_f);                                         \
        }                                                                 \
    } while (0)

    me_cache_stats_t before = {0};
    me_cache_stats(eng, &before);
    print_stats("before                ", &before);

    /* 1st render — populate cache. */
    RENDER_OR_FAIL("1st"); if (local_failure) { rc = 1; goto cleanup; }
    me_cache_stats_t after_1st = {0};
    me_cache_stats(eng, &after_1st);
    print_stats("after-1st-render-miss ", &after_1st);

    /* 2nd render — must hit (DiskCache or in-engine cache). */
    RENDER_OR_FAIL("2nd"); if (local_failure) { rc = 1; goto cleanup; }
    me_cache_stats_t after_2nd = {0};
    me_cache_stats(eng, &after_2nd);
    print_stats("after-2nd-render-hit  ", &after_2nd);

    /* Invalidate by asset hash — drops the AssetHashCache entry. */
    s = me_cache_invalidate_asset(eng, kAssetHash);
    if (s != ME_OK) {
        fprintf(stderr, "invalidate_asset: %s (%s)\n",
                me_status_str(s), me_engine_last_error(eng));
        rc = 1; goto cleanup;
    }
    me_cache_stats_t after_inv = {0};
    me_cache_stats(eng, &after_inv);
    print_stats("after-invalidate-asset", &after_inv);

    /* 3rd render — must miss. DiskCache side-index dropped the
     * derived graph-hash entry on invalidate_asset, so the engine
     * re-decodes from source. */
    RENDER_OR_FAIL("3rd"); if (local_failure) { rc = 1; goto cleanup; }
    me_cache_stats_t after_3rd = {0};
    me_cache_stats(eng, &after_3rd);
    print_stats("after-3rd-render-miss ", &after_3rd);

    /* Universal escape hatch — drops every cache layer. */
    s = me_cache_clear(eng);
    if (s != ME_OK) {
        fprintf(stderr, "cache_clear: %s (%s)\n",
                me_status_str(s), me_engine_last_error(eng));
        rc = 1; goto cleanup;
    }
    me_cache_stats_t after_clr = {0};
    me_cache_stats(eng, &after_clr);
    print_stats("after-cache-clear     ", &after_clr);

    /* 4th render — MUST miss (clear dropped everything). */
    RENDER_OR_FAIL("4th"); if (local_failure) { rc = 1; goto cleanup; }
    me_cache_stats_t after_4th = {0};
    me_cache_stats(eng, &after_4th);
    print_stats("after-4th-render-miss ", &after_4th);

    /* Verification ------------------------------------------------- */
    const long long entries_dropped =
        (long long)(after_2nd.entry_count - after_inv.entry_count);
    const long long misses_after_inv =
        (long long)(after_3rd.miss_count - after_inv.miss_count);
    const long long misses_after_clear =
        (long long)(after_4th.miss_count - after_clr.miss_count);
    fprintf(stderr,
            "  entries dropped by invalidate_asset:  %lld\n",
            entries_dropped);
    fprintf(stderr,
            "  miss_count delta across invalidate:   %lld\n",
            misses_after_inv);
    fprintf(stderr,
            "  miss_count delta across cache_clear:  %lld\n",
            misses_after_clear);
    if (entries_dropped >= 1 && misses_after_inv >= 1 &&
        misses_after_clear >= 1) {
        fprintf(stderr,
                "  OK: invalidate_asset dropped AssetHashCache entries + "
                "DiskCache derived frames (side-index); cache_clear forced "
                "a second re-decode.\n");
    } else {
        fprintf(stderr,
                "  FAIL: expected entries_dropped >= 1 && "
                "misses_after_inv >= 1 && misses_after_clear >= 1\n");
        rc = 1;
    }

cleanup:
    me_timeline_destroy(tl);
    me_engine_destroy(eng);
    return rc;
}
