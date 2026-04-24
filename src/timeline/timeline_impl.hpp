/* Internal timeline IR. Minimal — expands as schema v1 coverage grows.
 *
 * Phase-1 scope: single video track with N contiguous clips, no effects,
 * no transforms. Everything else is rejected at load time with
 * ME_E_UNSUPPORTED.
 *
 * Assets are first-class: `Timeline::assets` maps id → Asset, and clips
 * reference an asset by `asset_id` rather than duplicating the URI. This
 * keeps per-asset metadata (URI, content hash, and future colorSpace /
 * metadata bag) orthogonal to per-clip metadata (time window) — same
 * asset referenced by N clips stores only once.
 */
#pragma once

#include "media_engine/types.h"
#include "timeline/animated_number.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace me {

/* Per-asset color space override (TIMELINE_SCHEMA.md §Color). Each axis is
 * independently optional — `Unspecified` means "trust container metadata
 * for this axis". Populated into Asset only when JSON explicitly carries
 * `"colorSpace":{...}`; a missing object leaves Asset::color_space as
 * std::nullopt (= trust container for everything).
 *
 * M2 OCIO pipeline consumes these enums to pick a working-space transform.
 * Keep the enumeration stable with the schema string tables in the loader;
 * adding a value needs a matching entry in both directions. */
struct ColorSpace {
    enum class Primaries : uint8_t {
        Unspecified = 0, BT709, BT601, BT2020, P3_D65
    };
    enum class Transfer : uint8_t {
        Unspecified = 0, BT709, SRGB, Linear, PQ, HLG, Gamma22, Gamma28
    };
    enum class Matrix : uint8_t {
        Unspecified = 0, BT709, BT601, BT2020NC, Identity
    };
    enum class Range : uint8_t {
        Unspecified = 0, Limited, Full
    };

    Primaries primaries{Primaries::Unspecified};
    Transfer  transfer {Transfer::Unspecified};
    Matrix    matrix   {Matrix::Unspecified};
    Range     range    {Range::Unspecified};
};

struct Asset {
    std::string uri;          /* resolved later (strip file:// at I/O time) */

    /* Optional: lowercase hex sha256 digest (64 chars) of the asset bytes.
     * Sourced from JSON `contentHash` when present; empty means unknown
     * and the engine's AssetHashCache computes lazily on first need. */
    std::string content_hash;

    /* Optional override of container color-space metadata. Present iff
     * JSON asset object carried `"colorSpace":{...}`. M2-prep — M2 OCIO
     * is the only consumer. */
    std::optional<ColorSpace> color_space;
};

/* Snapshot of a Transform's 8 fields at a specific composition time.
 * Produced by `Transform::evaluate_at(t)`; consumed by the compose
 * loop / affine math. Identity defaults match Transform's defaults. */
struct TransformEvaluated {
    double translate_x  = 0.0;
    double translate_y  = 0.0;
    double scale_x      = 1.0;
    double scale_y      = 1.0;
    double rotation_deg = 0.0;
    double opacity      = 1.0;
    double anchor_x     = 0.5;
    double anchor_y     = 0.5;

    /* Spatial identity ⇔ translate == 0, scale == 1, rotation == 0.
     * Opacity and anchor don't participate — opacity is applied via
     * alpha_over (not spatial); anchor only matters when there's
     * rotation/scale. */
    bool spatial_identity() const {
        return translate_x  == 0.0 &&
               translate_y  == 0.0 &&
               scale_x      == 1.0 &&
               scale_y      == 1.0 &&
               rotation_deg == 0.0;
    }
};

/* 2D transform applied when the clip composites onto the output canvas.
 * Each field is an `AnimatedNumber` — supports `{"static": v}` and
 * `{"keyframes": [...]}` JSON forms (migrated by the
 * `transform-animated-support` bullet layer 3). Identity defaults make
 * `Transform{}` a valid "no-op" state.
 *
 * Caller pattern: `auto eval = clip.transform->evaluate_at(T);` — reads
 * 8 doubles into a `TransformEvaluated` at composition time T. Callers
 * that ignore T (e.g. preview-at-t=0 flat static) can pass
 * `me_rational_t{0, 1}`.
 *
 * Stored as std::optional<Transform> on Clip: nullopt = "JSON clip has
 * no `transform` key" (vs Transform{} = "transform key present with
 * all identity defaults"). The distinction lets downstream code
 * fast-path clips that truly omit transforms. */
struct Transform {
    AnimatedNumber translate_x  = AnimatedNumber::from_static(0.0);
    AnimatedNumber translate_y  = AnimatedNumber::from_static(0.0);
    AnimatedNumber scale_x      = AnimatedNumber::from_static(1.0);
    AnimatedNumber scale_y      = AnimatedNumber::from_static(1.0);
    AnimatedNumber rotation_deg = AnimatedNumber::from_static(0.0);
    AnimatedNumber opacity      = AnimatedNumber::from_static(1.0);
    AnimatedNumber anchor_x     = AnimatedNumber::from_static(0.5);
    AnimatedNumber anchor_y     = AnimatedNumber::from_static(0.5);

    TransformEvaluated evaluate_at(me_rational_t t) const {
        return TransformEvaluated{
            translate_x.evaluate_at(t),
            translate_y.evaluate_at(t),
            scale_x.evaluate_at(t),
            scale_y.evaluate_at(t),
            rotation_deg.evaluate_at(t),
            opacity.evaluate_at(t),
            anchor_x.evaluate_at(t),
            anchor_y.evaluate_at(t),
        };
    }
};

/* Media kind of a clip. Mirrors TIMELINE_SCHEMA.md §Clip `"type"` enum
 * values "video" and "audio". Text clips (M5) will extend this enum.
 * Loader enforces that a clip's type matches its parent track's kind
 * (no audio clips on a video track or vice versa). */
enum class ClipType : uint8_t {
    Video = 0,
    Audio = 1,
};

/* Typed effect parameter tagged union.
 *
 * VISION §3.2 forbids `Map<String, Float>`-shaped effect parameter
 * APIs. Each EffectKind has its own POD parameter struct; EffectSpec
 * holds a std::variant over them so add-a-new-kind is a variant
 * extension + a parse branch, not a map entry.
 *
 * Params are plain numbers (not AnimatedNumber) today — keyframed
 * effect params arrive with the `effect-param-animated` cycle once
 * GPU effects actually consume them. Schema doc lists all three
 * kinds' param names (TIMELINE_SCHEMA.md §Effect "Core kinds").
 *
 * Ranges documented here are *semantic* and not loader-enforced —
 * downstream GPU effects clamp to their shader's valid domain.
 * Loader only enforces "required params present, types correct". */
struct ColorEffectParams {
    double brightness = 0.0;   /* ~[-1, +1]; 0 = identity */
    double contrast   = 1.0;   /* ~[0, 2];   1 = identity */
    double saturation = 1.0;   /* ~[0, 2];   1 = identity */
};

struct BlurEffectParams {
    double radius = 0.0;       /* pixels; 0 = identity */
};

struct LutEffectParams {
    std::string path;          /* .cube file path / URI; asset_ref
                                * resolution deferred to LUT effect */
};

/* EffectKind enum. Stable once shipped — appending new kinds is ABI-
 * safe (new enum value + new variant alternative); reordering /
 * removing kinds is not. JSON tags ("color", "blur", "lut") live in
 * loader_helpers.cpp's dispatch; add entries in lock-step. */
enum class EffectKind : uint8_t {
    Color = 0,
    Blur  = 1,
    Lut   = 2,
};

struct EffectSpec {
    /* Optional JSON "id" for addressable effect updates (future M3+
     * scrub-time parameter tweaks). Empty when JSON omits "id". */
    std::string    id;

    EffectKind     kind{EffectKind::Color};

    /* `enabled` defaults true per TIMELINE_SCHEMA.md §Effect. Consumer
     * skips disabled effects entirely — cheaper than running through
     * `mix=0`. */
    bool           enabled{true};

    /* Blend factor between input and effect output. AnimatedNumber so
     * keyframed fades work on the same `{"static": v}` / `{"keyframes":
     * [...]}` shape as Transform fields. Default = full effect. */
    AnimatedNumber mix = AnimatedNumber::from_static(1.0);

    /* Typed params by EffectKind. The variant's index must match the
     * kind enum's underlying value (Color → 0, Blur → 1, Lut → 2) so
     * consumers can `std::get_if<ColorEffectParams>(&spec.params)`
     * without re-checking kind. Loader enforces the invariant. */
    std::variant<ColorEffectParams, BlurEffectParams, LutEffectParams>
        params{ColorEffectParams{}};
};

struct Clip {
    /* JSON clip id (from `{"id": ...}` in the timeline JSON). Unique
     * within a track (loader enforces). Used by transitions
     * (Transition::from_clip_id / to_clip_id reference this) and by
     * future effect/keyframe addressing. Non-empty after load. */
    std::string   id;

    /* Reference into Timeline::assets. Guaranteed non-empty after load;
     * loader rejects clips with unknown asset_id. */
    std::string   asset_id;

    /* Reference into Timeline::tracks by id. Stamped at load time by
     * the loader (walking tracks JSON). Keeps Timeline::clips a single
     * flat list while still letting multi-track consumers (compose
     * kernel, when it lands) group clips by track. Always non-empty
     * after load. */
    std::string   track_id;

    ClipType      type{ClipType::Video};

    me_rational_t time_start   { 0, 1 };
    me_rational_t time_duration{ 0, 1 };
    me_rational_t source_start { 0, 1 };

    /* Optional 2D transform. Populated iff JSON clip carries a
     * `transform` object (empty object = Transform{} with identity
     * defaults; absent = nullopt). Phase-1 is static-only; the loader
     * rejects the `keyframes` animated form. Valid on video clips
     * only — loader rejects transform on audio clips (semantically
     * meaningless). */
    std::optional<Transform> transform;

    /* Audio-only: optional animated gain in decibels. Populated iff
     * JSON clip carries a `gainDb` object of the form
     * `{"static": <number>}` or `{"keyframes": [...]}` (same
     * AnimatedNumber shape as Transform fields). Keyframe times are
     * in timeline-global rational time — consumers evaluate via
     * `gain_db->evaluate_at(T)` where T is the audio frame's
     * timeline-global timestamp. Ignored on video clips (loader
     * rejects gainDb on video). Consumer is AudioMixer (applies
     * per-emitted-frame linear gain). */
    std::optional<AnimatedNumber> gain_db;

    /* Ordered list of effects applied to this clip's rendered
     * output. Populated iff JSON clip carries a non-empty `effects`
     * array. Applied in declaration order — later effects see the
     * output of earlier ones (matches TIMELINE_SCHEMA.md §Clip
     * "effects (array, optional) — applied in order").
     *
     * Video-only today: loader rejects effects on audio clips (audio
     * effect chain lands with M4 audio polish).
     *
     * Consumer: ComposeSink's GPU branch once effect-gpu-* cycles
     * land — today the list is loaded and stored but no consumer
     * walks it. Kept here so the IR + JSON schema stays honest:
     * timeline JSON with effects now round-trips through the IR
     * instead of being rejected. */
    std::vector<EffectSpec> effects;
};

/* Kind of a compositing track. Audio tracks carry audio clips, video
 * tracks carry video clips; the loader enforces the match. The
 * eventual compose kernel uses Kind to decide whether to route this
 * track's frames through the video blend path (Video) or the audio
 * mix path (Audio). */
enum class TrackKind : uint8_t {
    Video = 0,
    Audio = 1,
};

/* Kind of a transition between two adjacent clips on the same track.
 * Phase-1 schema accepts only CrossDissolve; other kinds (wipe, dip-
 * to-black, slide, …) are future schema extensions and get
 * ME_E_UNSUPPORTED at load time. Enum values are stable ABI once
 * shipped; append new kinds, never reorder. */
enum class TransitionKind : uint8_t {
    CrossDissolve = 0,
};

/* Transition between two adjacent clips on the same track. Semantics:
 * the transition overlaps the boundary between `from_clip_id` (ends at
 * boundary) and `to_clip_id` (starts at boundary), cross-mixing over
 * `duration` of timeline time. Phase-1 enforces adjacency (to-clip
 * immediately follows from-clip in the track's JSON-declared clip
 * order) and positive duration bounded by both clips' own durations.
 *
 * `track_id` is stamped by the loader — redundant with either
 * clip's track_id but lets consumers group by track without a clip
 * lookup. `from_clip_id` / `to_clip_id` are the JSON clip ids
 * (Clip itself has no id field today; transition consumers resolve
 * them by walking Timeline::clips filtered by track_id in JSON order,
 * and matching the first occurrence of each id).
 *
 * The compose / mix kernel reads this at render time; phase-1
 * Exporter rejects any non-empty Timeline::transitions because neither
 * the multi-track-compose-kernel nor audio-mix-kernel are implemented
 * yet (tracked by cross-dissolve-kernel backlog item). */
struct Transition {
    TransitionKind kind{TransitionKind::CrossDissolve};
    std::string    track_id;
    std::string    from_clip_id;
    std::string    to_clip_id;
    me_rational_t  duration{0, 1};
};

/* Compositing track metadata. Clips live in the flat Timeline::clips
 * list stamped with track_id back-references; this struct carries just
 * the track-level attributes that aren't per-clip (id, enabled flag,
 * and the JSON declaration order which is preserved in the vector).
 *
 * Multi-track-video-compose (M2) consumes this: at any given timeline
 * time T, group active clips by track_id and composite in
 * Timeline::tracks vector order (bottom = tracks[0], top = tracks[N-1]).
 *
 * Phase-1 render path (passthrough / reencode sequential concat) still
 * asserts `tracks.size() == 1` at the Exporter layer — the compose
 * kernel itself is tracked by the `multi-track-compose-kernel` backlog
 * item. */
struct Track {
    std::string id;
    TrackKind   kind{TrackKind::Video};
    bool        enabled{true};
};

struct Timeline {
    me_rational_t frame_rate   { 30, 1 };
    me_rational_t duration     { 0, 1 };
    int           width        { 0 };
    int           height       { 0 };

    /* id → Asset. Lookup-only (never iterated), so std::unordered_map is
     * safe for determinism — iteration order would be undefined, but we
     * resolve by key. */
    std::unordered_map<std::string, Asset> assets;

    /* Compositing tracks, in JSON declaration order (bottom→top when
     * compose lands). For phase-1 (single-track passthrough/reencode)
     * this is always size() == 1. */
    std::vector<Track> tracks;

    /* Flat clip list across all tracks. Each Clip carries its track_id
     * so multi-track consumers can group. For phase-1 this is just
     * tracks[0]'s clips in time order. */
    std::vector<Clip> clips;

    /* Flat transition list across all tracks. Each Transition carries
     * its track_id so the compose kernel can group. Transitions within
     * a single track are stored in the JSON declaration order of that
     * track's `transitions[]` array. Phase-1 Exporter rejects any
     * non-empty transitions list (see src/orchestrator/exporter.cpp
     * gate + cross-dissolve-kernel backlog item). */
    std::vector<Transition> transitions;
};

}  // namespace me

struct me_timeline {
    me::Timeline tl;
};
