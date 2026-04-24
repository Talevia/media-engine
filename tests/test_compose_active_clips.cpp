/*
 * test_compose_active_clips — tripwire for me::compose::active_clips_at.
 *
 * The resolver is pure data-in data-out: Timeline + rational time →
 * vector<TrackActive>. This suite pins:
 *
 *   - Single-track concat (3 clips): queries hit the expected clip
 *     index + source_time at interior / boundary / out-of-range points.
 *   - Two-track with different durations: one track shorter than the
 *     other; active_clips_at returns 2 entries where both cover, 1
 *     where only one does, 0 when neither.
 *   - Half-open interval convention: T == clip end belongs to NEXT
 *     clip (or nothing) — boundary consistency across tracks.
 *   - Empty timeline: returns empty vector (no crash).
 *   - Track order in output == Timeline::tracks declaration order.
 *   - Rational arithmetic: source_time = source_start + (T - time_start)
 *     stays exact across non-trivial denominators.
 */
#include <doctest/doctest.h>

#include "compose/active_clips.hpp"
#include "timeline/timeline_impl.hpp"

#include <vector>

using me::compose::active_clips_at;
using me::compose::TrackActive;

namespace {

/* Build a 3-clip single-track timeline: c0 [0..60/30), c1 [60..120/30),
 * c2 [120..180/30). Each clip consumes its full source starting at 0. */
me::Timeline three_clip_single_track() {
    me::Timeline tl;
    tl.frame_rate = me_rational_t{30, 1};
    tl.duration   = me_rational_t{180, 30};
    tl.width = 1920; tl.height = 1080;
    tl.assets.emplace("a1", me::Asset{});
    tl.tracks.push_back(me::Track{"v0", me::TrackKind::Video, true});
    for (int i = 0; i < 3; ++i) {
        me::Clip c;
        c.asset_id     = "a1";
        c.track_id     = "v0";
        c.type         = me::ClipType::Video;
        c.time_start   = me_rational_t{i * 60, 30};
        c.time_duration= me_rational_t{60, 30};
        c.source_start = me_rational_t{0, 30};
        tl.clips.push_back(std::move(c));
    }
    return tl;
}

/* Two-track: v0 has 2-second clip, v1 has 1-second clip (both at t=0). */
me::Timeline two_track_different_lengths() {
    me::Timeline tl;
    tl.frame_rate = me_rational_t{30, 1};
    tl.duration   = me_rational_t{60, 30};
    tl.width = 1920; tl.height = 1080;
    tl.assets.emplace("a1", me::Asset{});
    tl.tracks.push_back(me::Track{"v0", me::TrackKind::Video, true});
    tl.tracks.push_back(me::Track{"v1", me::TrackKind::Video, true});
    /* v0: 2s */
    me::Clip v0c;
    v0c.asset_id = "a1"; v0c.track_id = "v0"; v0c.type = me::ClipType::Video;
    v0c.time_start = me_rational_t{0, 30};
    v0c.time_duration = me_rational_t{60, 30};
    v0c.source_start = me_rational_t{0, 30};
    tl.clips.push_back(std::move(v0c));
    /* v1: 1s */
    me::Clip v1c;
    v1c.asset_id = "a1"; v1c.track_id = "v1"; v1c.type = me::ClipType::Video;
    v1c.time_start = me_rational_t{0, 30};
    v1c.time_duration = me_rational_t{30, 30};
    v1c.source_start = me_rational_t{0, 30};
    tl.clips.push_back(std::move(v1c));
    return tl;
}

}  // namespace

TEST_CASE("active_clips_at: single-track, interior point hits expected clip") {
    const auto tl = three_clip_single_track();
    /* T = 30/30 = 1.0s — inside clip 0 which is [0, 60/30) = [0, 2s) */
    const auto act = active_clips_at(tl, me_rational_t{30, 30});
    REQUIRE(act.size() == 1);
    CHECK(act[0].track_idx == 0);
    CHECK(act[0].clip_idx  == 0);
    /* source_time = 0 + (30/30 - 0) = 30/30 */
    CHECK(act[0].source_time.num * 30 == 30 * act[0].source_time.den);
}

TEST_CASE("active_clips_at: single-track, T at clip boundary belongs to NEXT clip") {
    /* Half-open [start, end): T == end_of_c0 == start_of_c1 → belongs to c1.
     * The 60/30 boundary is exactly start of clip 1 (and end of clip 0). */
    const auto tl = three_clip_single_track();
    const auto act = active_clips_at(tl, me_rational_t{60, 30});
    REQUIRE(act.size() == 1);
    CHECK(act[0].clip_idx == 1);
    /* source_time = clip1.source_start (0) + (60/30 - 60/30) = 0 */
    CHECK(act[0].source_time.num == 0);
}

TEST_CASE("active_clips_at: single-track, T beyond timeline returns empty") {
    const auto tl = three_clip_single_track();
    /* T = 200/30 > tl.duration 180/30 */
    const auto act = active_clips_at(tl, me_rational_t{200, 30});
    CHECK(act.empty());
}

TEST_CASE("active_clips_at: single-track, T exactly at duration end returns empty") {
    const auto tl = three_clip_single_track();
    /* T = 180/30, which is end of c2 — half-open means no active. */
    const auto act = active_clips_at(tl, me_rational_t{180, 30});
    CHECK(act.empty());
}

TEST_CASE("active_clips_at: single-track, later clip resolves source_time offset") {
    const auto tl = three_clip_single_track();
    /* T = 150/30 = 5.0s — inside clip 2 which is [120/30, 180/30). */
    const auto act = active_clips_at(tl, me_rational_t{150, 30});
    REQUIRE(act.size() == 1);
    CHECK(act[0].clip_idx == 2);
    /* source_time = 0 + (150/30 - 120/30) = 30/30 */
    CHECK(act[0].source_time.num * 30 == 30 * act[0].source_time.den);
}

TEST_CASE("active_clips_at: two-track, both tracks cover T") {
    const auto tl = two_track_different_lengths();
    /* T = 10/30 ≈ 0.33s — both v0 (2s) and v1 (1s) are active. */
    const auto act = active_clips_at(tl, me_rational_t{10, 30});
    REQUIRE(act.size() == 2);
    /* Order: tracks[0] = v0 first, tracks[1] = v1 second. */
    CHECK(act[0].track_idx == 0);
    CHECK(act[1].track_idx == 1);
}

TEST_CASE("active_clips_at: two-track, only the longer track covers T") {
    const auto tl = two_track_different_lengths();
    /* T = 45/30 = 1.5s — only v0 still running (v1 ended at 30/30 = 1s). */
    const auto act = active_clips_at(tl, me_rational_t{45, 30});
    REQUIRE(act.size() == 1);
    CHECK(act[0].track_idx == 0);
}

TEST_CASE("active_clips_at: empty timeline returns empty") {
    me::Timeline tl;
    const auto act = active_clips_at(tl, me_rational_t{0, 30});
    CHECK(act.empty());
}

TEST_CASE("active_clips_at: source_start offset threaded into source_time") {
    /* Clip trims into the middle of the source: time [0..60/30),
     * source [30/30..90/30). At T = 20/30, source_time should be 50/30. */
    me::Timeline tl;
    tl.frame_rate = me_rational_t{30, 1};
    tl.duration   = me_rational_t{60, 30};
    tl.assets.emplace("a1", me::Asset{});
    tl.tracks.push_back(me::Track{"v0", me::TrackKind::Video, true});
    me::Clip c;
    c.asset_id = "a1"; c.track_id = "v0"; c.type = me::ClipType::Video;
    c.time_start   = me_rational_t{0, 30};
    c.time_duration= me_rational_t{60, 30};
    c.source_start = me_rational_t{30, 30};   /* trim in 1s */
    tl.clips.push_back(std::move(c));

    const auto act = active_clips_at(tl, me_rational_t{20, 30});
    REQUIRE(act.size() == 1);
    /* source_time = 30/30 + (20/30 - 0) = 30/30 + 20/30 — cross-compare to 50/30 */
    CHECK(act[0].source_time.num * 30 == 50 * act[0].source_time.den);
}

TEST_CASE("active_transition_at: transition window maps to [-dur/2, +dur/2) around boundary") {
    /* 2-clip single-track timeline with a 1-second cross-dissolve.
     * clip A: time 0..2s, clip B: time 2..4s. Transition duration
     * 30/30 = 1s. Window: [1.5s, 2.5s). */
    me::Timeline tl;
    tl.frame_rate = me_rational_t{30, 1};
    tl.duration   = me_rational_t{120, 30};
    tl.assets.emplace("a1", me::Asset{});
    tl.tracks.push_back(me::Track{"v0", me::TrackKind::Video, true});
    /* Clip A */
    {
        me::Clip c;
        c.id = "cA"; c.asset_id = "a1"; c.track_id = "v0";
        c.time_start    = me_rational_t{0, 30};
        c.time_duration = me_rational_t{60, 30};
        c.source_start  = me_rational_t{0, 30};
        tl.clips.push_back(std::move(c));
    }
    /* Clip B */
    {
        me::Clip c;
        c.id = "cB"; c.asset_id = "a1"; c.track_id = "v0";
        c.time_start    = me_rational_t{60, 30};
        c.time_duration = me_rational_t{60, 30};
        c.source_start  = me_rational_t{0, 30};
        tl.clips.push_back(std::move(c));
    }
    /* Transition */
    tl.transitions.push_back(me::Transition{
        me::TransitionKind::CrossDissolve, "v0", "cA", "cB",
        me_rational_t{30, 30},
    });

    /* T = 45/30 = 1.5s — start of window. */
    auto at_start = me::compose::active_transition_at(tl, 0, me_rational_t{45, 30});
    REQUIRE(at_start.has_value());
    CHECK(at_start->transition_idx == 0);
    CHECK(at_start->t == doctest::Approx(0.0f));

    /* T = 60/30 = 2s — midpoint of window. */
    auto at_mid = me::compose::active_transition_at(tl, 0, me_rational_t{60, 30});
    REQUIRE(at_mid.has_value());
    CHECK(at_mid->t == doctest::Approx(0.5f));

    /* T = 74/30 ≈ 2.467s — near end of window. */
    auto at_near_end = me::compose::active_transition_at(tl, 0, me_rational_t{74, 30});
    REQUIRE(at_near_end.has_value());
    CHECK(at_near_end->t == doctest::Approx((74.0f - 45.0f) / 30.0f));

    /* T = 40/30 = 1.333s — before window. */
    CHECK_FALSE(me::compose::active_transition_at(tl, 0, me_rational_t{40, 30}).has_value());

    /* T = 75/30 = 2.5s — exactly at window end (half-open → NOT in). */
    CHECK_FALSE(me::compose::active_transition_at(tl, 0, me_rational_t{75, 30}).has_value());

    /* T = 90/30 = 3s — past window. */
    CHECK_FALSE(me::compose::active_transition_at(tl, 0, me_rational_t{90, 30}).has_value());
}

TEST_CASE("active_transition_at: track without transitions returns nullopt") {
    me::Timeline tl;
    tl.tracks.push_back(me::Track{"v0", me::TrackKind::Video, true});
    me::Clip c; c.id = "cA"; c.track_id = "v0";
    c.time_start = me_rational_t{0, 30};
    c.time_duration = me_rational_t{60, 30};
    tl.clips.push_back(std::move(c));

    CHECK_FALSE(me::compose::active_transition_at(tl, 0, me_rational_t{30, 30}).has_value());
}

TEST_CASE("active_transition_at: invalid track_idx returns nullopt") {
    me::Timeline tl;
    CHECK_FALSE(me::compose::active_transition_at(tl, 0, me_rational_t{0, 1}).has_value());
    tl.tracks.push_back(me::Track{"v0", me::TrackKind::Video, true});
    CHECK_FALSE(me::compose::active_transition_at(tl, 99, me_rational_t{0, 1}).has_value());
}

TEST_CASE("active_clips_at: output order mirrors Timeline::tracks declaration order") {
    /* Swap track declaration order: v1 first, v0 second.  Both cover T.
     * active_clips_at[0] must be v1 (tracks[0]), [1] must be v0.
     * This pins the "bottom → top z-order = tracks declaration order"
     * contract — critical once the compose kernel starts consuming
     * the output. */
    me::Timeline tl;
    tl.frame_rate = me_rational_t{30, 1};
    tl.assets.emplace("a1", me::Asset{});
    tl.tracks.push_back(me::Track{"v1", me::TrackKind::Video, true});
    tl.tracks.push_back(me::Track{"v0", me::TrackKind::Video, true});
    /* Clips order in flat list doesn't matter — what matters is track_id
     * matching. Put v0 first in clips, v1 second, opposite of tracks. */
    me::Clip cv0;
    cv0.asset_id = "a1"; cv0.track_id = "v0"; cv0.type = me::ClipType::Video;
    cv0.time_start = me_rational_t{0, 30};
    cv0.time_duration = me_rational_t{60, 30};
    cv0.source_start = me_rational_t{0, 30};
    tl.clips.push_back(std::move(cv0));
    me::Clip cv1;
    cv1.asset_id = "a1"; cv1.track_id = "v1"; cv1.type = me::ClipType::Video;
    cv1.time_start = me_rational_t{0, 30};
    cv1.time_duration = me_rational_t{60, 30};
    cv1.source_start = me_rational_t{0, 30};
    tl.clips.push_back(std::move(cv1));
    tl.duration = me_rational_t{60, 30};

    const auto act = active_clips_at(tl, me_rational_t{10, 30});
    REQUIRE(act.size() == 2);
    /* tracks[0] is v1 per declaration, so act[0].track_idx == 0 → v1's clip. */
    CHECK(act[0].track_idx == 0);
    CHECK(tl.tracks[act[0].track_idx].id == "v1");
    CHECK(act[1].track_idx == 1);
    CHECK(tl.tracks[act[1].track_idx].id == "v0");
}
