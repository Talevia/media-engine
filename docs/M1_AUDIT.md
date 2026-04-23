# M1 audit — 2026-04-23

Written as part of `docs-m1-audit` backlog task. Purpose: compile evidence that each of M1's unchecked exit criteria in `docs/MILESTONES.md` is de-facto landed, so the user can make a one-time call to tick the boxes and advance "Current: " to M2. Per `iterate-gap` hard rule 9, this skill cannot auto-tick milestones — only the user does that, and only after reviewing this audit.

## M1 exit criteria — evidence table

MILESTONES.md has 10 exit criteria for M1. The first 3 were already ticked before this audit began (`me_engine_create/destroy`, `me_timeline_load_json` schema v1, `me_render_start` passthrough). The remaining 7 are addressed below.

| # | Criterion | Landing commit(s) | Test coverage | Verified on dev machine |
|---|---|---|---|---|
| 4 | `me_probe` 实装：container / codec / duration / W×H / 帧率 / sample_rate / channels 全部从 libavformat 拉 | `49ed302 feat(probe): implement me_probe via libavformat` (+ `8396ae2 me-probe-more-fields` adds rotation/color/bit_depth accessors) | `tests/test_probe.cpp` — 5 cases / 45 assertions: fixture-fed probe populates container/codec/dimensions/fps; extended accessors (rotation/color/bit_depth); null-arg rejection; ME_E_IO on missing URI; accessor null-tolerance. | `ctest` → `test_probe` green; `build/examples/04_probe/04_probe /tmp/input.mp4` prints all fields. |
| 5 | `me_thumbnail_png` 实装：任意 asset 指定时间点产 PNG | `156576d feat(thumbnail): thumbnail-impl — me_thumbnail_png via libavformat + PNG encoder` | `tests/test_thumbnail.cpp` — 6 cases / 30 assertions: native + bounded scaling + no-upscale + ME_E_IO on missing URI + null-arg rejection; PNG magic + IHDR W×H parsed inline. | `ctest` → `test_thumbnail` green; `build/examples/06_thumbnail/06_thumbnail ...` outputs valid PNG; ffprobe/file(1) identify as PNG. |
| 6 | `me_render_start` 新增至少 1 条 re-encode 路径（h264 via VideoToolbox） | `f9404f0 feat(orchestrator): reencode-h264-videotoolbox` + `2bfa6cd reencode-multi-clip` (N-segment concat) + `4ad072e debt-render-bitexact-flags` (byte-deterministic output) | `tests/test_determinism.cpp` case 3 "h264/aac reencode is byte-deterministic across two independent renders" — 242 KB output byte-identical across engine restarts (skipped cleanly on non-mac hosts without h264_videotoolbox). | `ctest` → `test_determinism` 3/3 cases green; `build/examples/05_reencode/05_reencode ...` produces valid h264/aac MP4 for both single- and multi-clip timelines. |
| 7 | `me_timeline_load_json` 支持单轨 N clip（concat / trim 组合） | `f1e290b feat(timeline): multi-clip-single-track — passthrough concat on one video track` | `tests/test_timeline_schema.cpp` case "multi-clip single-track with contiguous time ranges loads" (4s = 2 × 2s) + non-contiguous clip rejection; `tests/test_asset_reuse.cpp` covers 2-clip same-asset dedup. | `ctest` → `test_timeline_schema` + `test_asset_reuse` green; `01_passthrough` concats N clips into one output. |
| 8 | 单元测试框架接入（doctest），至少 1 条通过的 passthrough 确定性回归 | `fd5926d feat(tests): test-scaffold-doctest — doctest via FetchContent + first C API regressions` + `a0435df feat(tests): determinism-regression-test — passthrough byte-equality tripwire` | 12 doctest suites + 1 `gen_fixture` target; `test_determinism` 3 cases / 16 assertions covering both passthrough and reencode determinism. | `ctest --test-dir build` → 12/12 suites pass (159+ assertions cumulatively); `build/tests/test_determinism -s` verbose shows 16/16 assertions pass, 0 skipped on mac. |
| 9 | `me_cache_stats` 返回真实计数（hit/miss/entry_count 不全为 0，配合至少一层 asset 级缓存） | `a4a1c1c feat(cache): cache-stats-impl — wire stats/clear/invalidate to real state` | `tests/test_cache.cpp` 7 cases / 41 assertions: stats round-trip (seed → stats.count=1 → invalidate → count=0), clear, invalidate null-arg, codec_ctx_count from CodecPool, memory_bytes from FramePool. | `ctest` → `test_cache` green; `me_cache_stats` returns non-zero `entry_count` / `hit_count` / `miss_count` / `codec_ctx_count` in happy-path integration. |
| 10 | graph / task / scheduler / resource / orchestrator 五模块骨架就位；timeline 按段切分；passthrough 已迁到 Timeline → Exporter 执行路径 | `268c346 docs(architecture)` + `2045f43 feat(graph): graph-task-bootstrap` + `00a4459 feat(scheduler): taskflow-integration` + `8951ea2 feat(engine): engine-owns-resources` + `51f4963 feat(orchestrator): orchestrator-bootstrap` + `6fb4be3 feat(timeline): timeline-segmentation` + `f5dfa39 feat(io,orchestrator): refactor-passthrough-into-graph-exporter` | `tests/test_timeline_segment.cpp` 5 cases / 32 assertions covering segment boundary / boundary_hash determinism; `examples/02_graph_smoke` integration-tests graph→task→scheduler end-to-end; `examples/03_timeline_segments` covers segmentation. | `02_graph_smoke` prints correct values for 5 scenarios (diamond graph evaluates right, content_hash stable, kernel error propagates); `03_timeline_segments` prints expected segment breakdowns for 5 scenarios. `src/` has all 5 module subdirs (`graph/`, `task/`, `scheduler/`, `resource/`, `orchestrator/`) with populated implementations. |

## Summary

All 7 remaining M1 exit criteria have:
- a named landing commit (`git show <hash>` reproduces the work),
- CI test coverage (either doctest `tests/test_*.cpp` or integration `examples/0*/`),
- dev-machine verified end-to-end behavior (`ctest` green + example runs produce expected artefacts).

## What to do with this audit

1. Skim each row to sanity-check the landing commit + test coverage claim.
2. If satisfied: edit `docs/MILESTONES.md` to flip each of the 7 `- [ ]` boxes to `- [x]`.
3. Update the top-of-file `> **Current: M1 — API surface wired**` pointer to `> **Current: M2 — Multi-track CPU compose + color management**`.
4. Add a decision record at `docs/decisions/<YYYY-MM-DD>-milestone-advance-m2.md` capturing the tick-off event + seeding the M2 backlog (per `MILESTONES.md` header: "milestone 推进本身也是一次 `docs/decisions/` 记录"). A fresh `iterate-gap` repopulate can then surface M2 work items.
5. Delete this `docs/M1_AUDIT.md` in the same commit — it's a one-time evidence snapshot, not a long-lived doc. `git log --follow` preserves history if future review wants to see what shipped.

If any row's evidence looks weak, leave that box unchecked and add a specific follow-up task to the backlog via standalone `docs(backlog):` commit; the other rows can still advance.

## Non-audit items

- **PAIN_POINTS.md 2026-04-22 entry** (`me_output_spec_t` codec field typed union) — not an M1 exit criterion; stays recorded as ongoing architectural pain, not blocking M1 advance.
- **M2+ exit criteria** in MILESTONES.md (multi-track compose, audio mix, OCIO, Transform, Cross-dissolve, etc.) — out of scope for this audit.
