## 2026-04-23 — multi-track-active-clips-resolver：per-frame 多轨活动 clip 解析器（Milestone §M2 · Rubric §5.1）

**Context.** `multi-track-compose-sink-wire` bullet 承载 "将 `me::compose::alpha_over` 内核接到 render 路径端到端" 的所有 remaining 工作——非常大（ComposeSink 设计、N-demux 调度、RGBA↔YUV 转换、Exporter gate 翻、e2e 确定性测试）。同 `multi-track-compose-kernel` 的处理方式一致：再切一块最小独立 slice 进来本 cycle。

本 cycle 切的是 **scheduling helper**：给定 `me::Timeline` + 某个 timeline 时间 T，返回"此刻每个 track 上哪个 clip 正在 active + source 媒体时间"。这是 compose 路径的"上层数据驱动"，和 alpha_over 的"下层像素处理"正交。两者组合才能把 ComposeSink 拼起来。

Before-state grep evidence：

- `grep -rn 'active_clips\|TrackActive\|me::compose::' src/` 只命中 `alpha_over.*`——无多轨活动 clip 解析。
- `src/timeline/segmentation.cpp` 存在但只处理**单轨**的 flat clips，输出是"N 段时间连续 segment"，不是"按时间点抽多轨源帧"；multi-track compose 需要的是**逐帧按 track 分组**，和 segmentation 的 scope 不同。
- `src/orchestrator/exporter.cpp` 遍历 `tl_->clips` 线性迭代（line 66 `for (const auto& clip : tl_->clips)`），单轨 concat 假设——多轨会要求"at time T, which clips from which tracks to feed to encoder"的新询问能力。

**Decision.** 新建 `src/compose/active_clips.{hpp,cpp}`，pure data-in data-out 函数：

1. **Data shape**：
   ```cpp
   struct TrackActive {
       std::size_t   track_idx;    // index into Timeline::tracks
       std::size_t   clip_idx;     // index into Timeline::clips
       me_rational_t source_time;  // clip.source_start + (T - clip.time_start)
   };
   std::vector<TrackActive> active_clips_at(const me::Timeline&, me_rational_t time);
   ```
   - `track_idx` / `clip_idx` 用 index 而非 pointer/reference，让 caller 按需重访 IR（pointer 易失效、index 对 IR 改动鲁棒）。
   - `source_time` 预先算好（loader 约定 `source_start + (T - time_start)` 映射），caller 不用再跑 per-call rational 算术。

2. **Semantics（半开 `[start, start+duration)`）**：
   - T 落在 `[c.time_start, c.time_start + c.time_duration)` 内算 active。
   - 边界点 T = `c.time_start + c.time_duration` 不属于 c，属于下一 clip (如果存在且时间连续) or 什么都不属于。
   - Loader 强制 within-track non-overlap，所以 per track 最多 1 个 active clip。
   - Timeline 外时间（T ≥ tl.duration 或某 track 未覆盖） → 该 track 不出现在 output vector 中（NOT a placeholder nullopt；simply skipped）。

3. **Output order = `tl.tracks` declaration order**：
   - 这是 **bottom → top z-order** 约定（`tl.tracks[0]` 底层、`tl.tracks[N-1]` 顶层）。
   - Compose 内核会按顺序 iterate over `active_clips_at` 的返回，分别 alpha_over 到当前 dst。Pin 住 order 这一条是保证合成结果确定性的基础。
   - 有 test case 专门 swap 声明 order vs flat clip order 验证："tracks[0] 即使对应的 clip 在 clip list 排后面，依然先被 emit"。

4. **Implementation**：
   - Outer loop over `tl.tracks`（stable order）。
   - Inner loop over `tl.clips`（linear scan）：找 `track_id` 匹配 + 时间覆盖的那一个。
   - Found → emit TrackActive + break（单 track 最多 1 个）。
   - O(tracks × clips) 复杂度；phase-1 scale（几十 clip、几个 track）完全够用。未来可以 index by track_id 优化到 O(clips)，先不过度设计。

5. **Tests**（`tests/test_compose_active_clips.cpp`，10 TEST_CASE / 25 assertion）：
   - 单轨 interior point → 正确 clip_idx + source_time。
   - 单轨 T at clip boundary → 下一 clip（半开 convention 验证）。
   - 单轨 T 超 duration / T 恰等 duration → 空。
   - 单轨 later clip → source_time 正确偏移到 `clip.time_start` 之后。
   - 双轨不同长度：T 在两者都覆盖段 → 2 entries；T 在只长的那个段 → 1 entry。
   - 空 Timeline → 空 output，no crash。
   - source_start 非零（clip 裁源 head-trim）→ source_time = source_start + (T - time_start) 数学正确。
   - **Tracks 声明 order vs flat clips order 解耦**：两轨时 `tracks[0]="v1"` + `tracks[1]="v0"`，flat clips 里 v0 的先 v1 的后 —— output 仍然按 tracks 声明 order = v1 先 v0 后（z-order 权威）。

6. **Rational 算术 in-place**：
   - `r_lt(a, b)` / `r_le(a, b)` / `r_add` / `r_sub` 都基于 cross-multiply 整数比较（`a.num * b.den` vs `b.num * a.den`），loader 已保证 `den > 0`。
   - 无 normalization / 无 gcd 化简 ——  phase-1 denominators 限于 `{30, 48000, 90000}` 类，i64 乘积不会 overflow。

**Scope 留给 follow-up**。剩余 sink-wire 工作（也是这个 cycle **没**完成的）：

- `ComposeSink` 类：持有 `std::vector<ActiveClipResolver>` 和 N 个 `DemuxContext`，每 output frame 调 `active_clips_at(tl, t)` → 对每个 TrackActive 从 demux 抽 frame at source_time → sws-scale 到 RGBA8 → alpha_over 叠加 → RGBA 转 YUV → 送 videotoolbox encoder。
- Exporter gate 翻：`if (tracks.size() > 1)` 分支走 ComposeSink 而非 UNSUPPORTED。
- e2e 测试：2-track 实际 render + 字节确定性。

**Alternatives considered.**

1. **返回 `std::vector<std::optional<TrackActive>>`** 等长 of tracks，nullopt 代表该 track 无 active —— 拒：optional-in-vector 调用侧要 `.has_value()` check，增加噪声；skipped-entry 方案让 compose 内核 iterate 干净。track 没出现在 output 里就不画该层（默认语义正好）。
2. **返回 `std::unordered_map<track_id, TrackActive>`** —— 拒：map iteration 顺序 undefined，破坏 z-order 确定性（SKILL §3a rule 6）。vector + preserved order 是唯一正确选择。
3. **把 resolver 写成 `Timeline::active_at(time)` 成员函数** —— 拒：Timeline IR 是 POD-ish 数据结构，加 compose-specific 方法破坏 层次。compose-only 语义归到 `src/compose/` namespace 更正确。
4. **一次性返回整段时间的 "多轨 segmentation"**（所有 track 合并的时间段 + 每段的 active clip tuple） —— 拒：未来 `multi-track-segmentation` 是一个独立抽象层；本 cycle 只做"at time T"的点查询（ComposeSink 逐帧 query 是够用的 pattern）。批量 segmentation 的 scope 独立且更大。
5. **用 `std::span` + view-style API** 避免拷贝 TrackActive —— 拒：TrackActive 只 32 bytes，每 frame 几个 entry；vector by value 语义清；future SIMD-optimized loop 需要再考虑内存 layout。
6. **allow T == tl.duration 作最后 clip 的一部分（闭区间 `[start, end]`）** —— 拒：半开 `[start, end)` 是 NLE / DAW 标准；Premiere/FCP/DaVinci 都是 out-point exclusive。让 timeline 长度和合成输出长度 naturally 在 T = duration 结束。

业界共识来源：OTIO 的 `Timeline.find_children_by_time()` 返回多 active clip 的 API 模式；Premiere / FCP 的 timeline cursor model（at playhead position T, which clips are "at this frame" on each track）；Ableton Live 的 timeline-cursor 查询 (`tracks[i].clip_at_position(beat)`)。

**Coverage.**

- `cmake --build build` + `-Werror` clean。
- `ctest --test-dir build` 18/18 suite 绿（新 `test_compose_active_clips` 是第 18 个）。
- `build/tests/test_compose_active_clips` 10 case / 25 assertion / 0 fail。
- 其他 17 suite 全绿，无 side-effect（新 TU 独立，只新增 symbol）。

**License impact.** 无新 dep。

**Registration.**
- `src/compose/active_clips.{hpp,cpp}` 新 TU。
- `src/CMakeLists.txt` `media_engine` source list 追加 `compose/active_clips.cpp`。
- `tests/test_compose_active_clips.cpp` 新 suite。
- `tests/CMakeLists.txt` `_test_suites` 追加 + `target_include_directories` block。
- `docs/BACKLOG.md`：删 `multi-track-compose-sink-wire`（其 scope 中"resolver"部分本 cycle 完成），P1 末尾加新版 `multi-track-compose-sink-wire` （narrower，只剩 sink + gate flip + e2e）。

**§M 自动化影响.** M2 exit criterion "2+ video tracks 叠加" 本 cycle **未完成**——schema/IR（已）+ 合成内核（已）+ 调度 resolver（本 cycle 新加）三件已齐，但 sink 层未接通，Exporter gate 仍 UNSUPPORTED。§M.1 evidence check：`src/orchestrator/exporter.cpp` 的多轨 gate 没变；该 exit criterion 保留未打勾。下一 cycle 的 `multi-track-compose-sink-wire` 应能闭环（3 件拼起来 + sink 几百行 + e2e 一个大 test）。
