/*
 * 11_error_diagnosis — switch-on-status retry pattern.
 *
 * Most existing examples bail with a generic "rc != ME_OK" check
 * that loses the structured failure mode. Real hosts (talevia
 * scrub UI, agent-driven pipelines) need to distinguish:
 *
 *   - Transient I/O failures (ME_E_IO, ME_E_NOT_FOUND)
 *     → retry with backoff, surface stale-cache fallback.
 *   - Configuration errors (ME_E_PARSE, ME_E_INVALID_ARG)
 *     → bail; no retry helps a malformed JSON.
 *   - Capability gaps (ME_E_UNSUPPORTED)
 *     → fall through to a different codec / route.
 *
 * This example deliberately triggers each class against the same
 * engine handle, prints status + last_error, and demonstrates the
 * canonical switch-on-status block that hosts copy-paste into
 * their integration layer.
 *
 * Usage:
 *   11_error_diagnosis
 *
 * No arguments — all failures are self-contained (malformed JSON
 * literal, nonexistent file URI, unknown codec name). Exits 0
 * when all 3 expected failure modes were observed correctly.
 */
#include <media_engine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Categorize a status into one of the 3 host-actionable buckets.
 * The exact mapping is host policy — engine just hands you the
 * me_status_t. This shows the canonical shape. */
static const char* category(me_status_t s) {
    switch (s) {
    /* Recoverable: retry with backoff or fall back to a cache. */
    case ME_E_IO:
    case ME_E_NOT_FOUND:
        return "TRANSIENT (retry)";
    /* User-actionable: timeline / spec is wrong; tell the user. */
    case ME_E_PARSE:
    case ME_E_INVALID_ARG:
        return "CONFIG (bail)";
    /* Engine doesn't speak this dialect; try a different
     * codec / route / fall back to a software path. */
    case ME_E_UNSUPPORTED:
        return "CAPABILITY (fall through)";
    case ME_OK:
        return "OK";
    default:
        return "OTHER (log + bail)";
    }
}

static int expect_status(const char*    label,
                         me_status_t    expected,
                         me_status_t    got,
                         const char*    last_err) {
    fprintf(stdout, "  %s\n", label);
    fprintf(stdout, "    status   = %d (%s)\n",
            (int)got, me_status_str(got));
    fprintf(stdout, "    category = %s\n", category(got));
    fprintf(stdout, "    error    = %s\n",
            last_err && last_err[0] ? last_err : "<no message>");
    if (got != expected) {
        fprintf(stderr, "    FAIL: expected status %d (%s), got %d (%s)\n",
                (int)expected, me_status_str(expected),
                (int)got,      me_status_str(got));
        return 1;
    }
    return 0;
}

int main(void) {
    me_engine_t* eng = NULL;
    me_status_t  s   = me_engine_create(NULL, &eng);
    if (s != ME_OK) {
        fprintf(stderr, "engine_create: %s\n", me_status_str(s));
        return 1;
    }

    int failures = 0;

    /* --- Case 1: malformed JSON → ME_E_PARSE --------------------- */
    {
        const char* bad_json = "not-json";
        me_timeline_t* tl = NULL;
        s = me_timeline_load_json(eng, bad_json, strlen(bad_json), &tl);
        failures += expect_status(
            "malformed JSON → ME_E_PARSE",
            ME_E_PARSE, s, me_engine_last_error(eng));
        if (tl) me_timeline_destroy(tl);
    }

    /* --- Case 2: nonexistent source URI → ME_E_IO ---------------- *
     * me_probe is the cheapest path that surfaces an I/O failure
     * without going through me_render_start's full setup. */
    {
        me_media_info_t* info = NULL;
        s = me_probe(eng, "file:///definitely/does/not/exist.mp4", &info);
        failures += expect_status(
            "nonexistent file:// URI → ME_E_IO",
            ME_E_IO, s, me_engine_last_error(eng));
        if (info) me_media_info_destroy(info);
    }

    /* --- Case 3: unknown video codec → ME_E_UNSUPPORTED ---------- *
     * Build a minimal valid timeline + try to render with a bogus
     * codec name. The exporter rejects synchronously at
     * me_render_start because the codec registry doesn't know
     * "totally-not-a-codec". */
    {
        const char* bare_json =
            "{\"schemaVersion\":1,"
            "\"frameRate\":{\"num\":30,\"den\":1},"
            "\"resolution\":{\"width\":160,\"height\":120},"
            "\"colorSpace\":{\"primaries\":\"bt709\",\"transfer\":\"bt709\","
                            "\"matrix\":\"bt709\",\"range\":\"limited\"},"
            "\"assets\":[{\"id\":\"a0\",\"uri\":\"file:///tmp/none.mp4\"}],"
            "\"compositions\":[{\"id\":\"main\",\"tracks\":[{"
                "\"id\":\"v0\",\"kind\":\"video\",\"clips\":[{"
                    "\"id\":\"c0\",\"type\":\"video\",\"assetId\":\"a0\","
                    "\"timeRange\":  {\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":2,\"den\":1}},"
                    "\"sourceRange\":{\"start\":{\"num\":0,\"den\":1},\"duration\":{\"num\":2,\"den\":1}}"
                "}]}]}],"
            "\"output\":{\"compositionId\":\"main\"}}";
        me_timeline_t* tl = NULL;
        s = me_timeline_load_json(eng, bare_json, strlen(bare_json), &tl);
        if (s != ME_OK) {
            fprintf(stderr, "  setup load_json: %s\n", me_engine_last_error(eng));
            ++failures;
        } else {
            me_output_spec_t spec = {0};
            spec.path        = "/tmp/me_diag_unsupported.mp4";
            spec.container   = "mp4";
            spec.video_codec = "totally-not-a-codec";
            spec.audio_codec = "passthrough";
            me_render_job_t* job = NULL;
            s = me_render_start(eng, tl, &spec, NULL, NULL, &job);
            failures += expect_status(
                "unknown video codec → ME_E_UNSUPPORTED",
                ME_E_UNSUPPORTED, s, me_engine_last_error(eng));
            if (job) me_render_job_destroy(job);
            me_timeline_destroy(tl);
        }
    }

    me_engine_destroy(eng);

    if (failures > 0) {
        fprintf(stderr, "%d expected failure mode(s) did not match\n", failures);
        return 1;
    }
    fprintf(stdout, "all 3 failure categories observed as expected\n");
    return 0;
}
