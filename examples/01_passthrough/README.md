# 01_passthrough

End-to-end smoke test: read a timeline JSON, stream-copy the single clip's asset to MP4. No decode, no encode, no filter — just proof the ABI + JSON loader + FFmpeg I/O layer talk to each other.

## What's exercised

- `me_engine_create` / `me_engine_destroy`
- `me_timeline_load_json` (happy path + schema validation)
- `me_render_start` (async, worker thread), `me_render_wait`, `me_render_job_destroy`
- Progress callback (start / frames / completed | failed)

## Build & run

```sh
cmake -B build -S .                # first run fetches nlohmann_json
cmake --build build

# Grab any video file as input
cp /path/to/some_video.mp4 /tmp/input.mp4

build/examples/01_passthrough/01_passthrough \
    examples/01_passthrough/sample.timeline.json \
    /tmp/output.mp4

ffprobe /tmp/output.mp4
```

## Timeline constraints this demo enforces

The phase-1 loader rejects anything it can't handle with a clear `ME_E_UNSUPPORTED` + `me_engine_last_error` message:

- Exactly one composition (selected by `output.compositionId`)
- Exactly one video track, one video clip
- `clip.timeRange.start == 0`
- `clip.timeRange.duration == clip.sourceRange.duration`
- `clip.sourceRange.start == 0`
- No `effects`, no `transform`

Output must specify `video_codec = "passthrough"` and `audio_codec = "passthrough"`.

## ⚠️ FFmpeg licensing note

The system FFmpeg on your machine (likely Homebrew) is built with `--enable-gpl --enable-libx264 --enable-libx265`. Linking against it makes the resulting **example binary** GPL. That is fine for local development and for validating this demo.

**Production builds must use an LGPL FFmpeg**: no `--enable-gpl`, no `libx264` / `libx265`. Encoding flows through HW encoders (VideoToolbox, NVENC, …). See `docs/INTEGRATION.md` → "FFmpeg licensing at deploy time".
