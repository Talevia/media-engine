/* Internal timeline IR — composition structure.
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
 *
 * Layout: this header carries the composition-structure types (Asset,
 * Clip, Track, Transition, Timeline, me_timeline). Parameter PODs
 * (ColorSpace, Transform, ClipType, TextClipParams, SubtitleClipParams,
 * EffectSpec) live in `timeline_ir_params.hpp` and are included here;
 * TUs that only need parameter types (loader_helpers parsers) can
 * include that file directly.
 */
#pragma once

#include "media_engine/types.h"
#include "timeline/animated_number.hpp"
#include "timeline/timeline_ir_params.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace me {

/* Asset kind axis. Defaults to Media — existing JSON without a
 * `"type"` field continues to parse unchanged. ML asset kinds
 * (Landmark / Mask / Keypoints) are produced by an inference
 * runtime upstream of the engine; they reference a model via
 * `MlAssetMetadata` so the engine can content-hash + cache them
 * (M11 §137 contentHash key includes model_id / version /
 * quantization). */
enum class AssetKind : uint8_t {
    Media     = 0,   /* video / audio / image — the only kind shipped pre-M11. */
    Landmark  = 1,   /* Nx2 floats per frame + per-point confidence. */
    Mask      = 2,   /* alpha sequence (e.g. portrait segmentation). */
    Keypoints = 3,   /* skeleton with connectivity (e.g. body-pose track). */
};

/* Common ML-asset metadata — the M11 §136 schema requires every
 * non-Media asset to carry these three fields so the contentHash
 * key (M11 §137) can include them and the model lazy-load
 * callback (M11 §138) can validate licensing. */
struct MlAssetMetadata {
    std::string model_id;
    std::string model_version;
    std::string quantization;   /* e.g. "fp32" / "fp16" / "int8" */
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

    /* Asset kind. Defaults to Media; loader sets Landmark / Mask /
     * Keypoints when JSON carries `"type":"landmark"` etc. */
    AssetKind kind = AssetKind::Media;

    /* Required when kind != Media; nullopt for Media. Loader enforces
     * the kind→metadata invariant. */
    std::optional<MlAssetMetadata> ml_metadata;
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

    /* Populated iff `type == ClipType::Text`. Holds the per-clip
     * text rendering parameters (content + color + font / size /
     * position). Loader populates this from the clip JSON's
     * `textParams` object; absent / nullopt for non-text clips.
     * Consumer (future text_renderer): draws the text onto the
     * compose canvas at each output frame's timeline-global T,
     * evaluating the AnimatedNumber fields per-frame. */
    std::optional<TextClipParams> text_params;

    /* Populated iff `type == ClipType::Subtitle`. Carries the inline
     * subtitle markup (.ass / .srt) that the SubtitleRenderer
     * consumes at render time. Absent / nullopt for non-subtitle
     * clips. Consumer: compose_decode_loop's subtitle branch —
     * lazy-inits a SubtitleRenderer per clip_idx, calls
     * render_frame(t_ms) onto track_rgba, then the standard
     * opacity + alpha_over stages composite it on top of video. */
    std::optional<SubtitleClipParams> subtitle_params;
};

/* Kind of a compositing track. Mirrors TIMELINE_SCHEMA.md's `kind`
 * enum: "video" / "audio" / "text" / "subtitle". The loader enforces
 * per-track clip-type match. Text / subtitle tracks carry synthetic
 * clips (no source asset); video / audio tracks carry clips
 * referencing assets. */
enum class TrackKind : uint8_t {
    Video    = 0,
    Audio    = 1,
    Text     = 2,
    Subtitle = 3,
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
