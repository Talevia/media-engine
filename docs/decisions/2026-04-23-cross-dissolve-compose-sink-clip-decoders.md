## 2026-04-23 — cross-dissolve-compose-sink-clip-decoders（scope-A of cross-dissolve-transition-render-wire：ComposeSink 换成 per-clip decoder 索引）（Milestone §M2 · Rubric §5.1）

**Context.** Bullet `cross-dissolve-transition-render-wire` sub-scope (2) 要求 ComposeSink 循环在 transition 窗口内能同时 pull **from_clip** 和 **to_clip** 两个 decoder。现状：`ComposeSink::process` 按 **per-track** 开 decoder（`std::vector<TrackDecoderState> track_decoders(tl_.tracks.size())`，`src/orchestrator/compose_sink.cpp:167`），每 track 取 `first_clip_on_track(...)` 打开**单一** decoder——这条架构下 cross-dissolve 根本不可能写对：两个 clip 在同一 track 内，但 per-track 索引只有一个 slot。

再深一层，这个限制**也悄悄破坏了 multi-clip-single-track 在 compose 路径的支持**——当一个 track 有 2 个 adjacent clip（无 transition），当前 code 只开 clip[0] 的 decoder，drain 完后 clip[1] 时间段全是黑帧。`tests/test_compose_sink_e2e.cpp` 的 3 个测试都只用 1 clip per track，这个 latent bug 没暴露。

Before-state grep evidence：

- `src/orchestrator/compose_sink.cpp:167` 定义 `std::vector<TrackDecoderState> track_decoders(tl_.tracks.size())`——大小固定为 track 数。
- `src/orchestrator/compose_sink.cpp:169-170` open loop 用 `first_clip_on_track` 查每 track 的第一个 clip，剩余 clip 被忽略。
- `src/orchestrator/compose_sink.cpp:253` frame loop 用 `track_decoders[ta.track_idx]` 索引（注意是 track_idx，不是 clip_idx）。
- `grep -rn 'first_clip_on_track' src` 只有 compose_sink.cpp 这一处——本地辅助函数，删掉无跨文件影响。

**Decision.**

1. **`src/orchestrator/compose_sink.cpp`** 一个 scope-contained refactor：
   - `track_decoders[ti]` → `clip_decoders[ci]`，大小从 `tl_.tracks.size()` 改为 `tl_.clips.size()`。
   - Open 循环从 "for each track, open first clip's decoder" 改为 "for each clip, open that clip's decoder"：每 clip 一个 decoder state，索引对齐 `demuxes[ci]`（loader 已按 clip idx 开 demux，无需额外映射）。
   - Frame loop 的 lookup 从 `track_decoders[ta.track_idx]` 改为 `clip_decoders[ta.clip_idx]`——`TrackActive::clip_idx` 直接当索引，零换算。
   - 删掉 `first_clip_on_track` 辅助函数（-Werror 的 `-Wunused-function` 要求）。

2. **不做的事**（明确的 scope 边界）：
   - **不** wire `frame_source_at` 进 frame loop——继续用 `active_clips_at`。transition 路径的实现留给下一 cycle（dual-clip-pull cycle）。
   - **不**翻 Exporter 的 transition gate（`src/orchestrator/exporter.cpp:90` 仍 `return ME_E_UNSUPPORTED`）——wiring 未接，gate 翻开会暴露未实装路径。
   - **不** seek 支持——clip 的 `source_start > 0` 仍 unsupported（现有 compose 的既有限制，不属本 cycle scope）。
   - **不**新增测试 case——本 refactor 的正确性用 **现有 3 个 2-track compose e2e 测试 + 多轨 compose 全部 regression 维度**验证：lookup index 从 track_idx 换到 clip_idx 时，1-clip-per-track 的 timeline 两种索引返回同一个 decoder（该 track 上唯一那一个 clip），所以输出应**完全 byte-identical**——这就是回归覆盖。Multi-clip-per-track 的测试留到 dual-decoder wire-in cycle 一并做（单独加测 multi-clip-per-track compose 会先验证一个"refactor 副作用"而非当前目标，且该 case 暴露的 seek / source_start=0 假设 corner 和本 refactor 正交）。

3. **为什么本 refactor 是 bullet 的 prereq（而非本身完成 bullet sub-scope 之一）**：
   - Bullet sub-scope (1)(2)(3)(4) 都假设 ComposeSink 能 "从指定 clip_idx 的 decoder pull 一帧"。Pre-refactor：**不能**（只有"从 track_idx pull"）。本 refactor 把 "clip_idx → decoder" 这条访问路径打开。
   - 不做本 refactor 直接做 transition wire-in，要么在 ComposeSink 里平行维护第二个 transition-only decoder map（设计负担 + 在 transition 之外白跑），要么把 transition 路径硬塞进 per-track slot（会把 from_clip 的 decoder 挤掉，transition 结束恢复不回去）。
   - 本 refactor 的**外部行为不变**（现有所有测试 byte-identical），内部准备好接收下一 cycle 的 transition wire。这是教科书级别的 "make it easy to change → then make the change" Kent Beck 序式。

**Alternatives considered.**

1. **加一个 per-transition decoder cache 而非重构基础结构** —— 拒：两套 decoder 管理路径（per-track + per-transition）会让 ComposeSink 循环在判断"这一帧用哪个 decoder"时每行都得查两张表。统一 per-clip 一张表，frame_source_at 返回的 clip_idx 直接当 key 最简。
2. **Lazy-open（第一次 ta.clip_idx 命中时才 open decoder）** —— 拒：(a) 增加 "opened" 标记位 state；(b) 省的资源在 M2 典型 timeline（clip 数 ≤ 10）可忽略；(c) lazy 会让 decoder open 发生在 frame loop 中途，出错时诊断比上来一次性 open 麻烦。上来全开的 cost 是数 MB × clip 数，没到必须省的量级。
3. **把 `first_clip_on_track` 留在 anonymous namespace 以备未来用** —— 拒：YAGNI 违反 + -Werror 下未使用函数编译错；"未来用"等真需要再写不迟，留 dead code 是 2026-04-23 已有 `debt-*` bullet 明确不欢迎的劣化。
4. **把 dual-decoder infrastructure 做到 TrackDecoderState 内（struct 本身带 primary + secondary 两 slot）** —— 拒：TrackDecoderState 是"给定 (demux, stream_idx, decoder)"的 RAII 包装，semantic 是**一个** decoder。往里塞 secondary 等于给所有 caller（不只是 ComposeSink，还有 test_frame_puller）加了一个它们用不上的字段。正确层次是容器——ComposeSink 内有 `std::vector<TrackDecoderState>`，需要第二个 decoder 就多一条 entry，不改 struct 本体。
5. **一并做 multi-clip-per-track compose 的 e2e 测试** —— 拒：会把 source_start != 0 的 seek 问题拉进本 cycle，与 refactor 正交。留到下一 cycle 连同 transition wire-in 一起测。

**Scope 边界.** 本 cycle **不**做：
- frame loop 里 `frame_source_at` 的接入。
- Transition 路径的 2-decoder pull + cross_dissolve 调用。
- Exporter transition gate 翻转。
- multi-clip-single-track compose 的 seek 支持 / 回归测试。
- e2e cross-dissolve timeline 测试。

上述都在 bullet 剩余 scope 内，下一 cycle 继续。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 25/25 suite 绿。包括 3 个 test_compose_sink_e2e 测试（2-track compose、per-clip opacity、per-clip translate）——这些是 refactor 的回归覆盖：1-clip-per-track timeline 在 per-track 和 per-clip 索引下必须输出 byte-identical。
- 编译时变化：`-Wunused-function` 不再 trigger（`first_clip_on_track` 删除）；无其它 warning 出现。

**License impact.** 无。

**Registration.**
- `src/orchestrator/compose_sink.cpp`：Open 循环索引从 track_idx 切到 clip_idx；frame loop lookup 同步；`first_clip_on_track` 删除。
- `docs/BACKLOG.md`：bullet `cross-dissolve-transition-render-wire` 文本再一次 narrow——新增第 5 个 prereq "per-clip decoder indexing in ComposeSink"；剩余 sub-scope (1)(2)(3)(4) 描述更新为 "decoder lookup by clip_idx 已就位，只差 transition kind 分支 + 2 decoder pull + blend + gate"。**Bullet 不删**——核心 transition wire-in 仍未做。

**§M 自动化影响.** M2 exit criterion "Cross-dissolve transition" 本 cycle **未满足**——refactor 是 prereq，transition 实际渲染未接。§M.1 不 tick。

**SKILL.md §6 纪律说明.** 本 cycle 再次是 bullet 的 prereq slice（非 sub-scope 消耗），narrow bullet 文本。连续第二个 cycle 对同一 bullet 做这种操作——如果第 3 个 cycle 又是 prereq 而非真 wiring，说明 bullet 粒度太粗，下次 repopulate 时应拆分成独立 bullet。但这一次的 prereq 是**必须**的（没它 transition 写不了），且 refactor 对现有测试是 byte-identical 的——不算 "钻空子"，算真实的 step-by-step 推进。
