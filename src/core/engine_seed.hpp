/*
 * engine_seed — all the per-engine side effects a successful
 * `me_timeline_load_json` must trigger.
 *
 * Motivation: `me::timeline::load_json` is deliberately engine-agnostic
 * (it just parses a JSON string into the `me::Timeline` IR). But the
 * engine-facing C API entry point (`me_timeline_load_json`) has to
 * seed engine state from the loaded IR — today, the `AssetHashCache`
 * gets pre-warmed with each Asset's `contentHash`; future seeds (M2's
 * color pipeline pre-warm, M3's effect-LUT preheat) will pile on.
 *
 * Keeping these side effects inline in `src/api/timeline.cpp` would
 * mix "thin extern C glue" with "business side-effect trigger" in the
 * same body, and force every new seed consumer to touch the `api/`
 * layer. This header names the operation explicitly — `seed_engine_
 * from_timeline(engine, tl)` — so the extern C entry stays a glue
 * one-liner and every future seed consumer extends one function in
 * one TU.
 *
 * This is the lightweight resolution of backlog
 * `debt-timeline-loader-engine-seed-pattern`: it doesn't change the
 * loader's engine-agnostic signature (option (a) in the bullet) but
 * does give the side-effect set a first-class extension point (option
 * (b) in spirit). If future evidence demands (a) after all — e.g. a
 * seed operation that must influence parse decisions — the loader
 * signature can still be widened with minimal disruption.
 */
#pragma once

#include "core/engine_impl.hpp"
#include "timeline/timeline_impl.hpp"

namespace me::detail {

/* Apply every per-engine side effect that a successful Timeline load
 * should trigger. Called by `me_timeline_load_json` after the loader
 * returns ME_OK. Idempotent per (engine, timeline content): repeated
 * calls with the same inputs don't move observable state. */
void seed_engine_from_timeline(me_engine& engine, const me::Timeline& tl);

}  // namespace me::detail
