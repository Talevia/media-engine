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

#include <optional>
#include <string>
#include <unordered_map>
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

/* 2D transform applied when the clip composites onto the output canvas.
 * Phase-1 is static only: every field is a plain double, populated from
 * the `{"static": <number>}` form in JSON. The animated
 * (`{"keyframes": [...]}`) form is deliberately rejected by the loader
 * with ME_E_UNSUPPORTED so that this struct stays a stable shape while
 * animated-parameter support lands with M3. Defaults are identity so
 * `Transform{}` is a valid "no-op" state for code paths that read the
 * transform unconditionally.
 *
 * Stored as std::optional<Transform> on Clip: nullopt = "the JSON clip
 * has no `transform` key at all" (different from Transform{} which means
 * "transform key present but all fields defaulted to identity"). This
 * distinction lets downstream code optionally fast-path clips that truly
 * omit transforms. */
struct Transform {
    double translate_x  = 0.0;
    double translate_y  = 0.0;
    double scale_x      = 1.0;
    double scale_y      = 1.0;
    double rotation_deg = 0.0;
    double opacity      = 1.0;
    double anchor_x     = 0.5;
    double anchor_y     = 0.5;
};

/* Media kind of a clip. Mirrors TIMELINE_SCHEMA.md §Clip `"type"` enum
 * values "video" and "audio". Text clips (M5) will extend this enum.
 * Loader enforces that a clip's type matches its parent track's kind
 * (no audio clips on a video track or vice versa). */
enum class ClipType : uint8_t {
    Video = 0,
    Audio = 1,
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

    /* Audio-only: optional static gain in decibels. Populated iff
     * JSON clip carries a `gainDb` object of the form
     * `{"static": <number>}`. Phase-1 static-only; `keyframes` form
     * rejected by loader. Ignored on video clips (loader rejects
     * gainDb on video). Consumer is audio-mix-kernel (M2 follow-up)
     * and M4 audio-effect-chain. */
    std::optional<double> gain_db;
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
