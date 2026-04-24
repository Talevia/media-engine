# Timeline JSON schema

The one and only input format for `me_timeline_load_json`. No OTIO, no XML, no EDL — see VISION §4.

This document defines **schema v1**. Breaking changes require bumping `schemaVersion` and an explicit migration path.

## Top level

```json
{
  "schemaVersion": 1,
  "frameRate":  { "num": 30,   "den": 1 },
  "resolution": { "width": 1920, "height": 1080 },
  "colorSpace": { "primaries": "bt709", "transfer": "bt709", "matrix": "bt709", "range": "limited" },
  "workingColorSpace": "linear-rec709",
  "assets":       [ /* Asset */ ],
  "compositions": [ /* Composition */ ],
  "output":       { "compositionId": "main" }
}
```

- `schemaVersion` (int, required) — must be `1`. Loader rejects mismatch with `ME_E_PARSE`.
- `frameRate` (rational, required) — integer fractions; `den > 0`. Supports NTSC (`24000/1001`, `30000/1001`, `60000/1001`).
- `resolution` (required) — output canvas in pixels.
- `colorSpace` (required) — output color space. See §Color.
- `workingColorSpace` (optional, default `"linear-rec709"`) — internal compositing space.
- `assets` (array, optional) — referenced by `id` from clips.
- `compositions` (array, required, len ≥ 1) — named compositions.
- `output.compositionId` (string, required) — which composition the batch render produces.

## Rational numbers

All time, duration, and frame-rate values use `{num: int64, den: int64}`:

```json
{ "num": 100, "den": 3 }    // 33.333... seconds / 1/30 s / etc. depending on context
```

`den > 0` always. Zero duration = `{num: 0, den: 1}`. Negative time is invalid (loader rejects).

**Never use `double` seconds.** VISION §3.1 — determinism depends on exact arithmetic.

## Asset

```json
{
  "id": "a1",
  "kind": "video",
  "uri": "file:///Users/.../source.mp4",
  "contentHash": "sha256:9c1b...f2",
  "colorSpace": { "primaries": "bt709", "transfer": "bt709", "matrix": "bt709", "range": "limited" },
  "metadata": { }
}
```

- `id` (string, required, unique within timeline) — how clips refer to this asset.
- `kind` (enum, required) — `"video" | "audio" | "image"`.
- `uri` (string, required) — `file://`, `http://`, `https://`. Future: `content://` (Android), `s3://`, etc.
- `contentHash` (string, optional but strongly recommended) — `sha256:<hex>`. **Cache key component.** Without it, the engine re-hashes on open; with it, cache lookups short-circuit.
- `colorSpace` (object, optional) — override if the container metadata is wrong. Omit to trust container.
- `metadata` (object, optional, free-form) — opaque to the engine, passed through for host tooling.

## Composition

```json
{
  "id": "main",
  "duration": { "num": 300, "den": 1 },
  "tracks": [ /* Track */ ]
}
```

- `id` (string, required, unique within timeline).
- `duration` (rational, optional) — explicit composition duration. If omitted, derived as `max(clip.timeRange.end)` across all tracks.
- `tracks` (array) — evaluation order: later tracks composite over earlier.

## Track

```json
{
  "id": "v0",
  "kind": "video",
  "enabled": true,
  "clips": [ /* Clip */ ]
}
```

- `id` (string, required, unique within composition).
- `kind` (enum, required) — `"video" | "audio" | "subtitle" | "text"`.
- `enabled` (bool, optional, default `true`) — disabled tracks are parsed but skipped at render.
- `clips` (array) — must be ordered by `timeRange.start`, no overlap within a single track.

## Clip — common fields

```json
{
  "id": "c1",
  "timeRange":   { "start": { "num": 0, "den": 30 }, "duration": { "num": 60, "den": 30 } },
  "sourceRange": { "start": { "num": 0, "den": 30 }, "duration": { "num": 60, "den": 30 } },
  "transform": { /* Transform */ },
  "effects":   [ /* Effect */ ]
}
```

- `id` (required) — unique within composition.
- `timeRange` (required) — position on the timeline.
- `sourceRange` (required for media-backed clips; absent for `text`) — portion of source asset consumed.
- `transform` (optional) — 2D positioning. Default identity.
- `effects` (array, optional) — applied in order. Later effects see output of earlier ones.

### Video clip

```json
{
  "type": "video",
  "id": "c1",
  "assetId": "a1",
  "timeRange":   { ... },
  "sourceRange": { ... },
  "transform":   { ... },
  "effects":     [ ... ]
}
```

### Audio clip

```json
{
  "type": "audio",
  "id": "c2",
  "assetId": "a2",
  "timeRange":   { ... },
  "sourceRange": { ... },
  "gainDb":      { "static": 0.0 },
  "effects":     [ ... ]
}
```

- `gainDb` (animated number, optional, default `{"static": 0.0}`).

### Text clip

```json
{
  "type": "text",
  "id": "c3",
  "timeRange": { "start": {"num":0,"den":1}, "duration": {"num":2,"den":1} },
  "textParams": {
    "content":    "Hello",
    "color":      "#FFFFFFFF",
    "fontFamily": "Noto Sans SC",
    "fontSize":   { "static": 48 },
    "x":          { "static": 0 },
    "y":          { "static": 0 }
  }
}
```

Text clips live on tracks with `kind: "text"`. Unlike video / audio
clips, `assetId` and `sourceRange` are optional — text is rendered
from the `textParams` object directly with no source media to seek.

- `textParams.content` (string, required) — UTF-8 text.
- `textParams.color` (string, optional, default `#FFFFFFFF`) — CSS-like
  `#RRGGBB` or `#RRGGBBAA`. Named colors + 3-digit shorthand are not
  accepted.
- `textParams.fontFamily` (string, optional) — platform default when
  absent. Renderer falls back across available faces for glyphs the
  primary family lacks (CJK, emoji).
- `textParams.fontSize` / `x` / `y` (animated number, optional) — each
  keyframeable independently. Defaults: 48 / 0 / 0.

Loaded as of `text-clip-ir` (2026-04-24) — see
`src/timeline/timeline_ir_params.hpp`'s `TextClipParams` + loader
dispatch in `loader_helpers_clip_params.cpp::parse_text_clip_params`
(the helper TU was split by parse-shape as part of
debt-split-loader-helpers-cpp). Rendering integration (text →
canvas pixels) landed with `text-clip-render-skia` via
`me::text::TextRenderer`.

### Subtitle clip

```json
{
  "type": "subtitle",
  "id": "c4",
  "timeRange": { "start": {"num":0,"den":1}, "duration": {"num":5,"den":1} },
  "subtitleParams": {
    "content":  "1\n00:00:00,000 --> 00:00:01,000\nHello subs\n\n",
    "codepage": "cp1251"
  }
}
```

Subtitle clips live on tracks with `kind: "subtitle"`. Same shape as
text clips w.r.t. `assetId` / `sourceRange` being optional — the
renderer draws from `subtitleParams` directly via libass without
seeking source media.

- `subtitleParams.content` (string, required) — inline .ass / .ssa /
  .srt markup. Newlines embedded in JSON via the `\n` escape. libass
  parses the content once at clip entry and rasterises per-frame
  glyphs onto the composite canvas.
- `subtitleParams.codepage` (string, optional, default empty =
  UTF-8) — iconv codepage name for legacy .srt files whose bytes
  aren't UTF-8 (e.g. `cp1251`, `gbk`). Passed to libass's
  `ass_read_memory` codepage param. Empty / absent = UTF-8 assumed.

Loaded as of `compose-sink-subtitle-track-wire` (2026-04-24) — see
`src/timeline/timeline_ir_params.hpp`'s `SubtitleClipParams` + loader
dispatch in `loader_helpers_clip_params.cpp::parse_subtitle_clip_params`.
Compose-loop integration (draws onto `track_rgba` via
`me::text::SubtitleRenderer`, then alpha-overs onto the composite) is
wired in `src/orchestrator/compose_decode_loop.cpp` under
`ME_HAS_LIBASS`. Non-subtitle clips that carry a `subtitleParams`
field → `ME_E_PARSE` at load.

## Transform

All fields are animated properties. Defaults shown:

```json
{
  "translateX":  { "static": 0 },
  "translateY":  { "static": 0 },
  "scaleX":      { "static": 1 },
  "scaleY":      { "static": 1 },
  "rotationDeg": { "static": 0 },
  "opacity":     { "static": 1 },
  "anchorX":     { "static": 0.5 },
  "anchorY":     { "static": 0.5 }
}
```

Pixel space is output-canvas pixels; scale 1.0 = native clip size; anchor is in clip-local normalized coordinates.

## Effect

```json
{
  "id": "e1",
  "kind": "blur",
  "enabled": true,
  "mix":    { "static": 1.0 },
  "params": { "radius": 5.0 }
}
```

- `kind` (string, required) — registered effect kind. Unknown kind ⇒ `ME_E_UNSUPPORTED`.
- `id` (string, optional) — addressable handle for future scrub-time parameter tweaks; empty / absent is fine.
- `enabled` (bool, optional, default `true`) — disabled effects are skipped entirely (cheaper than `mix: 0`).
- `mix` (animated number, default `{"static": 1.0}`) — blends effect output with input; `0` = effect off, `1` = full effect.
- `params` (object, required) — **typed by `kind`**. Each kind has a parameter schema the loader validates. Type mismatch ⇒ `ME_E_PARSE`. Missing required field ⇒ `ME_E_PARSE`.

Loaded effect kinds (as of `timeline-loader-effect-parse`, 2026-04-24) — see `src/timeline/loader_helpers.cpp`'s `parse_effect_spec` dispatch:
- `color` — `brightness: number` (default 0), `contrast: number` (default 1), `saturation: number` (default 1). All optional; omitted fields use identity defaults.
- `blur` — `radius: number` (required).
- `lut` — `lutPath: string` (required). Asset-ref resolution is the LUT effect's responsibility; today it's a raw path string.

The IR carries these in `me::Clip::effects` as a typed `std::variant` by kind. GPU effect consumers land with follow-up `effect-gpu-*` bullets. `cross_dissolve` is a separate section (§Transition) — not a clip effect.

Audio clips reject non-empty `effects` today (`ME_E_PARSE`) — audio effect chain lands with M4 audio polish.

## Animated property

Two shapes, valid wherever a number/color/vec parameter is accepted:

**Static:**
```json
{ "static": 0.5 }
```

**Keyframed:**
```json
{
  "keyframes": [
    { "t": { "num": 0,  "den": 30 }, "v": 0.0, "interp": "linear" },
    { "t": { "num": 30, "den": 30 }, "v": 1.0, "interp": "bezier", "cp": [0.42, 0, 0.58, 1] },
    { "t": { "num": 60, "den": 30 }, "v": 1.0, "interp": "hold" }
  ]
}
```

- `t` (rational, required) — composition time (not clip-local).
- `v` (type-dependent, required) — number, `"#RRGGBB"` color, `[x, y]` vec2, etc.
- `interp` (enum, required) — `"linear" | "bezier" | "hold" | "stepped"`.
- `cp` (array of 4 floats, required when `interp == "bezier"`) — CSS cubic-bezier control points.

Before the first keyframe, the value is the first keyframe's `v`. After the last, the last keyframe's `v`. No extrapolation.

Keyframes must be sorted by `t`. Duplicate `t` values are invalid.

## Color

Color space is described as four enums:

```json
{
  "primaries": "bt709",
  "transfer":  "bt709",
  "matrix":    "bt709",
  "range":     "limited"
}
```

- `primaries`: `"bt709" | "bt601" | "bt2020" | "p3-d65"`
- `transfer`:  `"bt709" | "srgb" | "linear" | "pq" | "hlg" | "gamma22" | "gamma28"`
- `matrix`:    `"bt709" | "bt601" | "bt2020nc" | "identity"`
- `range`:     `"limited" | "full"`

Engine converts via OpenColorIO when `workingColorSpace` differs. Unsupported combinations ⇒ `ME_E_UNSUPPORTED`.

## Not yet supported

Explicitly rejected in schema v1 so callers know not to attempt:

- Nested compositions (pre-comps) — parser accepts only flat tracks for now. `composition` referenced as clip source will arrive post-phase-3.
- Transitions between clips on the same track — planned, not parsed.
- 3D transforms / camera — future schema version.
- Expressions (AE-style `thisComp.layer("x").transform...`) — out of scope.
- Masks / shape layers — post-phase-5.
- Time remapping within a clip (speed ramps) — phase 4.

Callers passing these fields get `ME_E_UNSUPPORTED`.

## Full example

A single 2-second video clip from an MP4, no effects, output as 1080p@30:

```json
{
  "schemaVersion": 1,
  "frameRate":  { "num": 30, "den": 1 },
  "resolution": { "width": 1920, "height": 1080 },
  "colorSpace": { "primaries": "bt709", "transfer": "bt709", "matrix": "bt709", "range": "limited" },
  "assets": [
    {
      "id": "a1",
      "kind": "video",
      "uri": "file:///tmp/input.mp4",
      "contentHash": "sha256:9c1b...f2"
    }
  ],
  "compositions": [
    {
      "id": "main",
      "tracks": [
        {
          "id": "v0",
          "kind": "video",
          "clips": [
            {
              "type": "video",
              "id": "c1",
              "assetId": "a1",
              "timeRange":   { "start": {"num":0,"den":30}, "duration": {"num":60,"den":30} },
              "sourceRange": { "start": {"num":0,"den":30}, "duration": {"num":60,"den":30} }
            }
          ]
        }
      ]
    }
  ],
  "output": { "compositionId": "main" }
}
```
