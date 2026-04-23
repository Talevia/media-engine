# C API contract

The headers in `include/media_engine/` are self-documenting. This file is the contract **around** them — ownership, threading, error handling, lifetime rules — that isn't obvious from reading function signatures.

If something in a header contradicts this document, the header is wrong; file a bug.

## Core concepts

### Engine

`me_engine_t*` is the root object. It owns:
- Worker thread pool (decode, filter, encode)
- GPU context (once the GPU backend lands)
- Frame / asset cache
- Thread-local last-error storage

Every other API call takes an engine handle. An engine is **thread-safe at the API boundary** — multiple threads may share one engine.

Create one engine per "hosting context" (e.g., per talevia `App`, per test). Many engines can coexist in one process.

### Timeline

`me_timeline_t*` is an **immutable parsed IR**. Loading is a one-shot operation; to "edit" a timeline, the host rebuilds JSON and re-loads. The engine may intern timeline objects internally, so re-loading a semantically identical JSON can return a cached IR cheaply.

Timelines are **thread-safe to read** concurrently. Multiple render jobs can share one timeline.

### Render job

`me_render_job_t*` is an active batch render. Work runs on engine worker threads; progress is delivered via callback on an engine-owned thread.

Jobs are **not thread-safe** — one caller owns the job until it reaches a terminal state. After `cancel` or `wait` returns, destroy the job.

## Error handling

Every fallible call returns `me_status_t`. `ME_OK` (0) is success; any other value is an error.

`me_status_str(status)` returns a terse English description (static storage, safe to pass around).

For the full human-readable error message (file paths, FFmpeg errors, schema violations), call `me_engine_last_error(engine)`. This is **thread-local per engine** — each calling thread has its own last-error slot. Valid until the next engine call on the same thread.

```c
me_timeline_t* tl;
me_status_t s = me_timeline_load_json(eng, buf, len, &tl);
if (s != ME_OK) {
    fprintf(stderr, "load failed: %s (%s)\n",
            me_status_str(s), me_engine_last_error(eng));
    return;
}
```

## Ownership rules

Rule of thumb: **if an API call hands you a pointer via an out-param, you own it and must destroy it.** Exceptions are explicit (e.g., pointers returned from query functions are owned by their parent handle).

| Returned as | Freed by | Lifetime |
|---|---|---|
| `me_engine_t**`         | `me_engine_destroy`       | until destroyed |
| `me_timeline_t**`       | `me_timeline_destroy`     | until destroyed |
| `me_render_job_t**`     | `me_render_job_destroy`   | until destroyed (after terminal state) |
| `me_frame_t**`          | `me_frame_destroy`        | until destroyed |
| `me_media_info_t**`     | `me_media_info_destroy`   | until destroyed |
| `uint8_t**` from `me_thumbnail_png` | `me_buffer_free` | until freed |
| `const char*` from query | (none)                   | until parent handle destroyed or next call on parent |
| `const uint8_t*` from `me_frame_pixels` | (none) | until `me_frame_destroy` |

**Strings are UTF-8, NUL-terminated, immutable.** Do not modify strings returned by the engine.

## Threading contract

| Surface | Safe concurrent from multiple threads? |
|---|---|
| `me_engine_*` calls | Yes |
| `me_timeline_*` read calls | Yes |
| `me_render_job_*` on different jobs | Yes |
| `me_render_job_*` on the **same** job | **No** — single owner |
| `me_frame_*` / `me_media_info_*` queries | Yes (read-only) |
| Callback invocation | Engine-internal thread; do not block it |

Callbacks (`me_progress_cb`) fire on **engine worker threads**, not the caller's thread. Marshal to your own thread if you need it (signal condvar, enqueue to channel, post to runloop, etc.). **Never** block inside a progress callback — it starves the render.

`me_engine_last_error` is **thread-local per engine**: thread A's error does not clobber thread B's view.

## String encoding

- All strings passed in and out are **UTF-8**.
- Path strings are UTF-8 too; on Windows we convert to UTF-16 internally. Do not pass local-code-page strings.
- `size_t` byte lengths include no trailing NUL unless the doc says otherwise.

## Common patterns

### Minimal render

```c
me_engine_t* eng;
if (me_engine_create(NULL, &eng) != ME_OK) return -1;

size_t len; char* json = read_file("timeline.json", &len);
me_timeline_t* tl;
me_status_t s = me_timeline_load_json(eng, json, len, &tl);
free(json);
if (s != ME_OK) { /* handle */ }

me_output_spec_t out = {
    .path = "out.mp4", .container = "mp4",
    .video_codec = "h264", .audio_codec = "aac",
};

me_render_job_t* job;
s = me_render_start(eng, tl, &out, on_progress, /*user=*/NULL, &job);
me_render_wait(job);
me_render_job_destroy(job);

me_timeline_destroy(tl);
me_engine_destroy(eng);
```

### Frame-server scrub

> **Stubbed until M6.** `me_render_frame` currently returns `ME_E_UNSUPPORTED`; the frame-server path + `me_frame_*` accessor bodies land with the M6 milestone. The shape below is the committed API surface so host code can be written against it today (the `if (... == ME_OK)` branch silently won't fire until the impl lands).

```c
me_rational_t t = { 30, 30 };   /* 1.0 s */
me_frame_t* f;
if (me_render_frame(eng, tl, t, &f) == ME_OK) {
    upload_to_texture(me_frame_pixels(f), me_frame_width(f), me_frame_height(f));
    me_frame_destroy(f);
}
```

### Probe asset metadata

```c
me_media_info_t* info;
if (me_probe(eng, "file:///path/to/clip.mp4", &info) == ME_OK) {
    printf("container=%s codec=%s %dx%d @ %lld/%lld fps\n",
           me_media_info_container(info),
           me_media_info_video_codec(info),
           me_media_info_video_width(info),
           me_media_info_video_height(info),
           (long long)me_media_info_video_frame_rate(info).num,
           (long long)me_media_info_video_frame_rate(info).den);
    /* Extended fields for M2 compose + OCIO: rotation, color_range,
     * color_primaries, color_transfer, color_space, bit_depth — see
     * probe.h. */
    me_media_info_destroy(info);
}
```

### Cache observability

```c
me_cache_stats_t stats;
if (me_cache_stats(eng, &stats) == ME_OK) {
    printf("assets cached: %lld  (hit=%lld miss=%lld)\n",
           (long long)stats.entry_count,
           (long long)stats.hit_count,
           (long long)stats.miss_count);
}

/* Drop a specific asset from the content-hash cache (after a host-side
 * file modification, for example). Accepts both "sha256:<hex>" and bare
 * "<hex>" forms. */
me_cache_invalidate_asset(eng, "sha256:5d41402abc4b2a76b9719d911017c592c3a1f0e8adf2eb0a0f8b5c7f3ec6f3a9");

/* Nuke all cache state (used at process shutdown or test teardown). */
me_cache_clear(eng);
```

### Progress callback

```c
static void on_progress(const me_progress_event_t* ev, void* user) {
    /* runs on an engine thread — don't block, don't reenter the engine
     * synchronously with long-running work. */
    switch (ev->kind) {
    case ME_PROGRESS_FRAMES:    signal_ui(ev->ratio); break;
    case ME_PROGRESS_COMPLETED: signal_done(ev->output_path); break;
    case ME_PROGRESS_FAILED:    signal_err(ev->status); break;
    default: break;
    }
}
```

## ABI stability

Public headers are C11 / `extern "C"`. The rules:

1. Enum values never change once assigned. New values are appended.
2. Struct layouts are append-only **for POD config structs** (e.g., `me_output_spec_t`) — new fields added at the end; callers zero-initialize before setting.
3. Function signatures are frozen once shipped. Evolution goes through `me_foo2(...)` or a new function.
4. Opaque handle layouts are internal; never depend on `sizeof(me_engine_t)`.

ABI-breaking changes bump the major version, ship a CHANGELOG entry, and land in a dedicated PR (never bundled with feature work).

## Language bindings

The C API is designed to be trivially bindable. Concrete how-tos live in `INTEGRATION.md`:

- JVM (JNI)
- Kotlin/Native (cinterop)
- Swift (C interop via module.modulemap)
- Python (ctypes / cffi / pybind11)

Rule for bindings: **don't leak C types into idiomatic host APIs**. Wrap `me_engine_t*` in a Kotlin class with a `close()`; wrap status codes in a sealed hierarchy; wrap progress callbacks in a `Flow<>`.
