## 2026-04-23 — cross-dissolve-active-transition-scheduler（scope-A of transition-wire：window resolver + Clip::id）（Milestone §M2 · Rubric §5.1）

**Context.** `cross-dissolve-transition-wire` bullet 的 full scope：scheduler 抽出 which transition is active at time T → decode from + to clips → run `cross_dissolve` 混合 → feed encoder → Exporter gate flip + e2e。这是多 cycle 工程。本 cycle 切 scope-A：**transition 活跃性查询** pure helper——`active_transition_at(tl, track_idx, T)` 返回 `{transition_idx, t}` 或 nullopt。类比 `active_clips_at` 的 scope-A 先行模式。

Before-state grep evidence：

- `grep -rn 'active_transition\|cross_dissolve' src/compose/` 只命中 `cross_dissolve.cpp`（kernel math）；没 scheduler。
- `me::Clip` struct 有 `track_id` 但**无** `id` 字段——loader 读 JSON clip id 是 local 变量只用于 intra-file validation，不持久化。Transition 里的 `from_clip_id` / `to_clip_id` 是 JSON 字符串 id，compose 层查不到对应 Clip。

**Decision.**

1. **`me::Clip` 加 `std::string id` 字段**（`src/timeline/timeline_impl.hpp`）：
   - Loader 在 clip construction 后 fill `c.id = clip_id`（clip_id 是原本 loader 读 JSON `"id"` 字段的 local 变量；移到 push_back 之前）。
   - Transition 消费者（compose scheduler）可以用 `id` match `Transition::from_clip_id` 找到对应 Clip。
   - 无 ABI impact（`me::Clip` 是内部类型，不 expose 到 C API）。

2. **`src/compose/active_clips.hpp/cpp` 扩 `active_transition_at`**（同 TU——"compose 层 per-track 按 T 查询" 是一个抽象层）：
   - 签名：`std::optional<ActiveTransition> active_transition_at(const me::Timeline&, std::size_t track_idx, me_rational_t time)`。
   - `struct ActiveTransition { size_t transition_idx; float t; }`——caller 用 `transition_idx` 查 Transition 自身，`t ∈ [0, 1]` 作 cross_dissolve kernel 的 lerp 参数。
   - 语义：对每 track 最多一个 active transition（loader 强制 adjacency + 无 within-track overlap）。
   - Window 采 **symmetric overlap**：`[from_clip.time_end - duration/2, from_clip.time_end + duration/2)`，半开区间。`t = (T - window_start) / duration`，float 除（rational 结果统一用 float 因为 kernel 接 float）。
   - `track_idx` 越界 → nullopt；track 上无 transition → nullopt。
   - 无 from_clip match（理论不可能；defensive）→ 跳该 transition。

3. **Tests**（`tests/test_compose_active_clips.cpp`，+3 TEST_CASE，10→13 cases，25→38 assertions）：
   - 构造 2-clip (2s+2s) 单轨 timeline + 1s cross-dissolve。Window = [1.5s, 2.5s)。
     - `T = 45/30 = 1.5s` → t ≈ 0.0（window start）。
     - `T = 60/30 = 2.0s` → t ≈ 0.5（中点）。
     - `T = 74/30 ≈ 2.467s` → t ≈ (74 - 45)/30 ≈ 0.967。
     - `T = 40/30 ≈ 1.333s` → nullopt（在 window 前）。
     - `T = 75/30 = 2.5s` → nullopt（半开区间，= end 不 in）。
     - `T = 90/30 = 3.0s` → nullopt（过 window）。
   - Track 上无 transition → nullopt。
   - `track_idx` 越界（empty tracks vector 或 idx=99）→ nullopt。

**Why symmetric overlap and not asymmetric**: 两种合理方案：
- **Symmetric**（本 cycle 采）：transition 居中在 clip boundary。from-clip 渲染到 `time_end - duration/2`，transition 持续 duration 秒，to-clip 从 `time_start + duration/2` 开始。需要 clips 的 sourceRange 有 head/tail handle。
- **Asymmetric**：transition 紧接 from-clip 结尾起，持续 duration 秒，只占 to-clip 开头。from-clip 在 `time_end` 截止，不需要 tail handle。

Loader 已保证 `transition.duration ≤ min(from.duration, to.duration)`，所以对 symmetric window 来说 duration/2 ≤ from.duration/2 + to.duration/2，从 from 的尾段取 duration/2 秒 + 从 to 的头段取 duration/2 秒数学上都在 clip 范围内，不需要额外的 head/tail handles 概念（只是 source 采样位置略有调整）。symmetric 是业界主流（Premiere "Default Transition Duration" 配置默认 symmetric "centered on cut"），选它。

**Follow-up bullet**: `cross-dissolve-transition-render-wire`——真把 scheduler + kernel 接到 render path。scope：ComposeSink::process 在每 output frame 调 `active_transition_at(tl, ti, T)`，transition 活跃时：从 from_clip 的 decoder pull 一帧 at `source_time = from.source_start + (from.time_end - T - duration/2)` 附近（反推回 source），从 to_clip 类似，cross_dissolve → alpha_over 到 dst，encode。Exporter `transitions gate` 翻。e2e test。

**Alternatives considered.**

1. **本 cycle 同时做 scheduler + ComposeSink 接入** —— 拒：ComposeSink 的 per-track-decoder model 假设 1 track 1 decoder；transition 需要"一 track 上同时 2 clips decoding"——显著 scheduler + decoder lifecycle 变化；两 cycle 拆开更稳。
2. **scheduler 放到 `src/orchestrator/` 而非 `src/compose/`** —— 拒：pure rational-time data query，和 `active_clips_at` 同层抽象；放 compose namespace 正确。
3. **return `Transition*` 而非 `transition_idx`** —— 拒：index 对 IR 改动鲁棒；pointer 可能悬垂如果 caller hold 结果跨 Timeline 改动。
4. **asymmetric window semantic** —— 拒：symmetric 是业界惯例（AE / Premiere / FCP 默认）；asymmetric 看起来"偏"。
5. **t 用 `me_rational_t`** 而非 float —— 拒：kernel（`cross_dissolve`）接 float；保持一致避免上层转换。
6. **查 from_clip 时用 linear scan on tl.clips** —— O(transitions * clips)。拒 index 预计算（复杂度增）——phase-1 规模小（< 100 clips），linear scan 几 µs。
7. **同时加 Clip::id 作 strong enum** —— 拒：id 是 JSON string 直接用；enum 要 registry 过度设计。

业界共识来源：AE / Premiere / FCP 的"centered on cut" default transition 语义；NLE industry 对 duration 的 "clip A 尾 + clip B 头各贡献 duration/2" 约定。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 25/25 suite 绿。
- `test_compose_active_clips` 13 case / 38 assertion（先前 10/25；+3 case / +13 assertion）。
- 现有 12 个 suite 全绿（`Clip::id` 添加是 additive 字段；`id` 默认空字符串，unit tests 没 set 它也能 compile+跑；loader test 的 JSON 都已经有 `"id"` 字段所以 Loader 永远 set）。

**License impact.** 无。

**Registration.**
- `src/timeline/timeline_impl.hpp`：`me::Clip` 新 `std::string id` 字段。
- `src/timeline/timeline_loader.cpp`：`c.id = clip_id`（1 行 move）。
- `src/compose/active_clips.{hpp,cpp}`：`struct ActiveTransition` + `active_transition_at()` 声明 + 实装。
- `tests/test_compose_active_clips.cpp`：+3 TEST_CASE。
- `docs/BACKLOG.md`：更新 `cross-dissolve-transition-wire` bullet（scheduler 就位，剩 render integration）；重命名更精确：`cross-dissolve-transition-render-wire`。

**§M 自动化影响.** M2 exit criterion "Cross-dissolve transition" 本 cycle **未完成**——scheduler + kernel 齐，但 render path integration 没。保留未打勾。
