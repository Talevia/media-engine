/*
 * Player-internal rational helpers — extracted from player.cpp's
 * anonymous namespace as part of `debt-split-player-cpp` so the
 * audio-producer / video-producer / pacer loop bodies can live in
 * their own TUs without each repeating the same arithmetic.
 *
 * Visibility: `src/orchestrator/`-internal only — not exported
 * through any public header. All functions are `inline` so the
 * single-instance ODR-merging across the player_*.cpp split TUs
 * matches the original anon-namespace single-TU behaviour without
 * silently growing the symbol table per consumer (`static` would
 * give per-TU copies).
 *
 * Consumed by: `player.cpp`, `player_audio_producer.cpp`. Future
 * splits (`player_producer.cpp` for video + `player_pacer.cpp`)
 * also include this header.
 *
 * Why these are a separate header vs inline in `player.hpp`:
 * `player.hpp` is the engine-internal API consumers see; helpers
 * here are TU-implementation glue and shouldn't widen the apparent
 * surface of Player.
 */
#pragma once

#include "media_engine/types.h"

#include <cstdint>
#include <numeric>

namespace me::orchestrator::player_detail {

/* Rational addition with reduction. Iterated addition (producer
 * cursor advances by frame_period each frame) would otherwise
 * explode the denominator — at 30 fps × 60 s the cumulated
 * denominator can overflow int64 around frame ~12 without GCD
 * reduction. Cross-multiply on int64 num/den; promoting to double
 * here would smuggle in float imprecision against which
 * docs/VISION.md §3.1 explicitly cautions. */
inline me_rational_t r_add(me_rational_t a, me_rational_t b) {
    if (a.den <= 0) a.den = 1;
    if (b.den <= 0) b.den = 1;
    int64_t num = a.num * b.den + b.num * a.den;
    int64_t den = a.den * b.den;
    if (den != 0) {
        const int64_t g = std::gcd(num < 0 ? -num : num, den);
        if (g > 1) { num /= g; den /= g; }
    }
    return me_rational_t{ num, den };
}

/* sign(a - b). Cross-multiplied on __int128 to absorb the products
 * of moderate-magnitude num/den pairs without overflow. */
inline int r_cmp(me_rational_t a, me_rational_t b) {
    if (a.den <= 0) a.den = 1;
    if (b.den <= 0) b.den = 1;
    const __int128 lhs = static_cast<__int128>(a.num) * b.den;
    const __int128 rhs = static_cast<__int128>(b.num) * a.den;
    if (lhs < rhs) return -1;
    if (lhs > rhs) return  1;
    return 0;
}

/* (a - b) in microseconds, clamped to int64. The clamp keeps
 * pacer scheduling sane when a wildly-out-of-range cursor is
 * passed (e.g. mid-shutdown the producer thread sees a stale
 * value); a 9-billion-µs ceiling is ~150 minutes — well past any
 * sensible drift but bounded so downstream wait_for budgets
 * don't underflow. */
inline int64_t r_micros_diff(me_rational_t a, me_rational_t b) {
    if (a.den <= 0) a.den = 1;
    if (b.den <= 0) b.den = 1;
    const __int128 num = static_cast<__int128>(a.num) * b.den
                       - static_cast<__int128>(b.num) * a.den;
    const __int128 den = static_cast<__int128>(a.den) * b.den;
    if (den == 0) return 0;
    const __int128 micros = (num * 1'000'000) / den;
    if (micros >  9'000'000'000LL) return  9'000'000'000LL;
    if (micros < -9'000'000'000LL) return -9'000'000'000LL;
    return static_cast<int64_t>(micros);
}

/* frame_period from frame_rate. {fps_num, fps_den} → {fps_den, fps_num}.
 * Defaults to 1/30 s when caller passes a degenerate rate. */
inline me_rational_t frame_period_from_rate(me_rational_t fr) {
    if (fr.num <= 0 || fr.den <= 0) return me_rational_t{1, 30};
    return me_rational_t{ fr.den, fr.num };
}

}  // namespace me::orchestrator::player_detail
