## 2026-04-23 — milestone-advance-m3：M2 全部 exit criteria 达成，推进 Current → M3（Milestone §M2 → §M3）

**Context.** §M.2 自动触发：M2 的 6 个 exit criteria 本 session 全部打勾。下一 milestone 是 M3 "Animated params + GPU backend (Metal first)"。

**M2 完成 snapshot.**

| Exit criterion | Landing commit | CI coverage |
|---|---|---|
| 2+ video tracks 叠加，alpha / blend mode 正确 | `5148848` (tick) + `bf0cc41` (multi-track-compose-actual-composite) | test_compose_alpha_over 11/37, test_compose_active_clips 17/56, test_compose_sink_e2e 6 case |
| OpenColorIO 集成，源/工作/输出空间显式转换，支持 bt709/sRGB/linear | `41de3f1` (tick) + `db59523` (ocio-colorspace-conversions) | test_color_pipeline OCIO branch |
| Transform（静态）端到端生效（translate/scale/rotate/opacity） | `af5a31a` (tick) + `b994fc9` (compose-transform-affine-wire) + `4eb3429` + `320e130` | test_compose_affine_blit 8/105, test_compose_sink_e2e per-clip opacity + translate cases |
| Cross-dissolve transition | `1ecf807` (tick) + `427fea2` (cross-dissolve-transition-render-wire) + 4 prereq cycles | test_compose_cross_dissolve 9/28, test_compose_active_clips active_transition_at, frame_source_at, test_compose_sink_e2e cross-dissolve case |
| 2+ audio tracks 混音，带 peak limiter | `0269659` (tick) + `4e1b386` (synthetic-tone-tests) + 8 prereq cycles（kernel→resample→fixture→pull_audio→track_feed→scheduler→builder→sink-wire） | test_audio_mix 15/49, test_audio_resample 9/177, test_audio_track_feed 8/1288, test_audio_mixer 14/104956, test_compose_sink_e2e video+audio case |
| 确定性回归测试：同一 timeline 跑两次产物 byte-identical（软件路径） | `22a8880` (tick) + `a812790` (compose-determinism-regression-test) | test_determinism 5 case（含新 compose-path 2-track byte-match 315429 bytes） |

累计 ~26 cycles 在本 session 内推进 M2 至完成，包括 video-compose / color / transform / cross-dissolve / audio-mix / determinism 六大主线 + 多次 debt / 基础设施 slice。

**Decision.**

1. **`docs/MILESTONES.md`** "Current: " 指针从 `M2` 改到 `M3`：
   - Pre: `> **Current: M2 — Multi-track CPU compose + color management**`
   - Post: `> **Current: M3 — Animated params + GPU backend (Metal first)**`

2. **`docs/BACKLOG.md`** bootstrap seed（§M.2 步骤 3）：
   - **移动**（P2 → P1 末尾）：
     - `transform-animated-support` —— 已存在的 P2 bullet 带 `Milestone §M3` 标签，scope 是解 loader 对 `{"keyframes":[...]}` 的硬拒 + 加 `me::AnimatedNumber` 类型（直接覆盖 M3 exit criterion "所有 animated property 类型的插值正确"）。
   - **新增**（为 M3 未覆盖 exit criteria）：
     - `bgfx-integration-skeleton` —— M3 "bgfx 集成，macOS Metal 后端可渲染" 的起步。grep `'bgfx\|Metal'` 返回 `docs/ARCHITECTURE.md` references only，`src/` 无 bgfx 代码，CMake 无 bgfx FetchContent。需要新 `src/gpu/bgfx_context.cpp` + CMake FetchContent + ARCHITECTURE.md 白名单更新。
     - `effect-registry-api-skeleton` —— M3 "EffectChain 能把连续 ≥ 2 个像素级 effect 合并成单 pass" + "≥ 3 个 GPU effect" 的基础。grep `'Effect\|effect' include/media_engine/*.h` 空。需要新 C API `me_effect_kind_t` enum + typed `me_effect_params_t` union（VISION §3.2 typed params 硬规）+ 内部 `me::effect::Effect` / `EffectChain` 抽象。没这一层其它 GPU effect 无处挂。

3. **不移动**的 P2 bullets（保持原档）：
   - `codec-pool-real-pooling` — `Milestone §M4-prep`，不属于 M3。
   - `async-job-base` — `Milestone §M6-prep`，不属于 M3。
   - `me-output-spec-typed-codec-enum` — `Milestone §M3-prep`，prep 指向 M3 邻近但不是 M3 正式任务（M3-M4 落第 3、4 个 codec 时评估）；等 M3 实际触发时视情况提升。暂保留 P2。

4. **无 `docs/M2_AUDIT.md`** —— 没有生成过一次性 audit snapshot（M2 的每条 criterion 都在其 tick commit + decision doc 里留痕，无需独立 audit 文件）。本 commit 不涉及删除 audit。

5. **§M.3 不触发**：新 milestone 的 P0+P1 bullet 数 = 3（transform-animated-support + bgfx-integration-skeleton + effect-registry-api-skeleton），>= 3 阈值，无需本 cycle repopulate。下 `/iterate-gap` 正常 pick top P1 自动进入 M3 工作。

**Alternatives considered.**

1. **本 cycle 同时做一次完整 §R repopulate（15 bullets）** —— 拒：§M.3 明文 "M.2 之后若 new milestone 的 P0 + P1 bullet 数 < 3 → 不在本 cycle 里立刻 repopulate（避免同 cycle 三连 commit 过长）"。我们达到 3 条，跳过 repopulate 符合纪律。
2. **Seed 更多 M3 bullet（覆盖全部 5 条 exit criteria）** —— 拒：`effect-registry-api-skeleton` 的 API 形状决定后，上面长 blur/color-correct/LUT 是具体 effect 的挂架子——等 skeleton 落地再 seed 具体 effect bullet 更准确（避免猜错 API）。"1080p@60 realtime" 是 benchmark 任务，等 GPU backend 出来再说。
3. **保留 `me-output-spec-typed-codec-enum` P2 不动 vs 提到 P1** —— 选"不动"：该 bullet 的 trigger 是"落第 3/4 个 codec"，不是 M3 本身任务。M3 的 codec stack 不变（仍 h264/aac + passthrough + 新加的 maybe SW-h264），不到 "3+ codec 痛" 的阈值。
4. **推进 cycle 同时顺手做个 cross-milestone cleanup（把 M2 decision file archive 到 subdir 之类）** —— 拒：scope creep。milestone-advance 专注推进指针 + seed backlog，cleanup 是独立 cycle。

**Coverage.**
- 本 cycle 不改代码，`cmake` + `ctest` 不触发。MILESTONES.md + BACKLOG.md + 本 decision 文件三件事一个 commit。

**License impact.** 无。

**Registration.**
- `docs/MILESTONES.md`：Current 指针 M2 → M3。
- `docs/BACKLOG.md`：
  - P1 新增 `bgfx-integration-skeleton` + `effect-registry-api-skeleton`。
  - P2 `transform-animated-support` 移到 P1 末尾（原 P2 第 4 条）。
  - 其它 P2 bullet 保留不动。
- `docs/decisions/2026-04-23-milestone-advance-m3.md`：本文件。
