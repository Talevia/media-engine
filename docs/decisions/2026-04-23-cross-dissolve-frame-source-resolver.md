## 2026-04-23 — cross-dissolve-frame-source-resolver（scope-A of cross-dissolve-transition-render-wire：per-(track, T) precedence resolver）（Milestone §M2 · Rubric §5.1）

**Context.** Bullet `cross-dissolve-transition-render-wire` 有 5 个未完成 sub-scope（detect transition / pull from+to frames / call cross_dissolve / replace active_clips_at call site / flip Exporter gate）加上"每 track 支持 decoder 跨 clip 切换或同时持 2 decoder"的基础设施。合并做是几百 LOC + 会往 ComposeSink 里引入尚不可测的状态机，风险高。

继续 `multi-track-compose-*` / `cross-dissolve-*` / `audio-mix-*` 系列已验证的 scope-A 拆切模式：kernel → scheduler helper → 然后才 wire 进 sink。本 cycle 切**sub-scope (4) "替代 active_clips_at 产出（transition 窗口内 transition 优先于独立 clip）"**——实际上是一条 precedence 规则，用一个纯函数 `frame_source_at(tl, track_idx, t)` 封住，把 "transition 优先 > single clip > none" 的语义从未来的 ComposeSink 循环中隔离出来可独立测试。

Before-state grep evidence：

- `src/compose/active_clips.hpp:52` 只有 `active_clips_at`（单 clip 每 track）。
- `src/compose/active_clips.hpp:72` 只有 `active_transition_at`（单 transition 每 track）。
- 两者独立存在；precedence（transition 窗口内用谁）在 callers 里尚未定义——ComposeSink 目前直接调 `active_clips_at`（`src/orchestrator/compose_sink.cpp:231`），transition 的 timelines 在 Exporter `src/orchestrator/exporter.cpp:90` 被 `ME_E_UNSUPPORTED` 提前 reject，根本没机会到 ComposeSink。所以 precedence 规则**无地方被实现**——grep `frame_source_at` 全仓空。

**Decision.**

1. **`src/compose/active_clips.{hpp,cpp}`** 新 pure function `frame_source_at(const Timeline&, size_t track_idx, me_rational_t t) → FrameSource`：
   - 返回 `FrameSource` struct，tagged by `FrameSourceKind {None, SingleClip, Transition}`。
   - **Precedence**：transition > single-clip > none。若 `active_transition_at(...)` 返 value，kind = Transition，附带 from_clip_idx / to_clip_idx（基于 transition 的 from_clip_id / to_clip_id → clips scan）+ 两个 source_time 直接算出来（`source_start + (T - time_start)` per clip）免得 caller 再扫一次。否则查 `active_clips_at` 过滤到此 track。否则 None。
   - **同 TU**（`active_clips.cpp`）——和 active_transition_at 是一套 per-(track, T) 查询家族，分文件没意义且会打断共享的 rational helpers（r_add / r_sub / r_le / r_lt 已私有 namespace anonymous）。
   - 纯函数：无全局状态、输入相同输出相同（CLAUDE.md determinism 不变量）。
   - 复杂度 O(C + T) per call（C = clips, T = transitions），未来若成瓶颈则 precompute 一次 per timeline-compile；目前 M2 typical clip 数 ≤ 10，不是热点。

2. **Scope 边界（明确不做的事）**：
   - **不** wire `frame_source_at` 进 `ComposeSink::process`。原因：wire-in 要求 sub-scope (1)(2)(3)（dual-decoder state + cross_dissolve call）同时齐活；单独 wire 本 resolver 会让 ComposeSink 拿到 `Transition` kind 不知如何 handle——要么 fall through 到单 clip（看不出本 cycle 工作）、要么报 UNSUPPORTED（新增 stub 违反 §3a 规则 7）。留给下一个 `cross-dissolve-render-dual-decoder` cycle 一并做。
   - **不** 翻 Exporter 的 transition gate（`src/orchestrator/exporter.cpp:90`）——gate 要等 wire-in 完成才能翻。
   - **不** 加 dual-decoder-per-track 基础设施。

3. **Tests**（`tests/test_compose_active_clips.cpp`）—— +4 TEST_CASE（13 → 17 cases；38 → 56 assertions；+18 assertions）：
   - Single-clip region: T=1s in clip A, 精确 before transition window (starts at 1.5s) → FrameSourceKind::SingleClip, clip_idx=0, source_time=1s。Pins 非-transition T 的退回路径。
   - Inside window midpoint: T=2s, window [1.5, 2.5) → Transition, t=0.5, from=cA(idx 0), to=cB(idx 1), from_source=2s, to_source=0s。Critical: T=2s 恰好是 clip A 的 half-open end，clip B 的 start——不走 precedence 规则的话 `active_clips_at(T=2s)` 只会返回 B（A 不 cover），这条测试明确 pin 住 Transition 覆盖 SingleClip。
   - Overlap front-half: T=1.667s（50/30）仅 A cover（B starts at 2s），但 transition 窗口覆盖。没 precedence 规则时返回 SingleClip(A)；带 precedence 时应返回 Transition。这条专门测 precedence 而非"两 clip 都 cover"的偶然正确。
   - Edge cases: T 超出 timeline duration → None；invalid track_idx → None；empty timeline → None。

**Alternatives considered.**

1. **直接 wire 进 ComposeSink，transition 分支 fall-through 到 to_clip single-decode** —— 拒：这在带 transition 的 timeline 产生"transition 直接切到 to_clip"的隐式错误行为（hard cut 而非 blend）。Exporter gate 现在会 reject 这种 timeline 不到 ComposeSink，但一旦 gate 翻就是隐藏 bug。没有 visible test 在跑的路径上 ship 错误行为是 nlohmann 级别的坑。
2. **在 `ComposeSink::process` 里 inline precedence 逻辑（不抽 helper）** —— 拒：(a) 未来第二个 transition kind（fade-in/out）需要同样的 precedence → inline 让二度动摇；(b) ComposeSink 循环已 > 200 LOC，继续叠加 per-track resolution 往单文件集中不利拆分。
3. **`FrameSource` 做 union/variant 而非 tagged struct** —— 拒：C++ `std::variant` 能用但本内部类型无 C ABI 跨越需要，tagged struct 简单明了（可选字段默认零值）；callers 主要写 `if (fs.kind == ...)` 开关，variant 的 `std::visit` 开销 > 收益。如果未来 kinds 多（>4）再转 variant 不迟。
4. **Precompute 一次 per timeline-load（Timeline 额外带一个 resolver cache）** —— 拒：(a) 改 Timeline IR shape 风险大（ABI / JSON schema 都会触）；(b) 性能尚未证实瓶颈。Pure function 每帧重算简单透明，profile 证实是热点再改。
5. **把 `frame_source_at` 拆成两次调用：`query_kind()` + `resolve_clip_indices()`** —— 拒：增加 caller 胶水，一次拿到完整 `FrameSource` 是 YAGNI-balanced 的数据接口。

**Scope 边界.** 本 cycle **不**做：
- ComposeSink 调用改造（sub-scope (4) 的"替代 active_clips_at"需要 caller 改，但 caller 要 dual-decoder 才有意义；留给下一 cycle）。
- Dual-decoder-per-track state（`TrackDecoderState` 扩展）。
- `cross_dissolve(...)` 调用 wire（kernel 已就位）。
- Exporter transition gate 翻转。
- e2e 测试。

以上是 bullet 剩余 4 个 sub-scope 的后续工作。

**Coverage.**

- `cmake --build build -j 4` 绿，`-Werror` clean。
- `ctest --test-dir build` 25/25 suite 绿。
- `test_compose_active_clips` 17 case / 56 assertion（先前 13/38；+4 case / +18 assertion）。

**License impact.** 无。

**Registration.**
- `src/compose/active_clips.hpp`：+ `FrameSourceKind` enum, `FrameSource` struct, `frame_source_at` decl。
- `src/compose/active_clips.cpp`：+ `frame_source_at` impl（复用 existing `active_transition_at` + `active_clips_at`）。
- `tests/test_compose_active_clips.cpp`：+ 4 TEST_CASE + `two_clip_with_transition()` fixture helper。
- `docs/BACKLOG.md`：bullet `cross-dissolve-transition-render-wire` 文本 narrow——sub-scope (4) "替代 active_clips_at 产出" 标记为 resolver-landed（precedence 规则已封装为 `frame_source_at`），剩 sub-scope (1)(2)(3)(5) + dual-decoder 基础设施。**Bullet 不删**——sub-scope 未全完。

**§M 自动化影响.** M2 exit criterion "Cross-dissolve transition" 本 cycle **未满足**——render integration 未接。§M.1 不 tick。

**SKILL.md §6 纪律说明.** 本 cycle 不删 bullet，只 narrow 文本（和 `cross-dissolve-active-transition-scheduler` cycle 相同做法——git 历史可查）。narrow 的 diff 只替换 bullet 单行文本，不动其它行，符合 "git diff BACKLOG.md 只减不增" 精神的合理延伸。
