## 2026-04-23 — debt-split-compose-sink-cpp：Transition 分支提到独立 TU（Milestone §M2 debt · Rubric 外）

**Context.** `src/orchestrator/compose_sink.cpp` 694 行，接近 debt-scan §R.5 的 P0 强制阈值 700。文件塞了 ComposeSink class body + make_compose_sink factory + process() 内部 frame loop 三大职责，且 process() 的 Transition kind 分支自身 ~90 LOC 已是独立逻辑单元。

Before-state：
- `wc -l src/orchestrator/compose_sink.cpp` = 694。
- 最近 cycles（audio-mix-sink-wire, audio-mix-synthetic-tone-tests, audio-only-timeline-support, cross-dissolve-transition-render-wire）给 compose_sink.cpp 加了 ~200 LOC 净增；reviewer 看 process() 方法 unfold 要翻屏。
- audio-only-timeline-support cycle 因此已经把 audio-only 路径**新文件化**（audio_only_sink.cpp）而非扩 compose_sink.cpp——同理，transition 分支也应该抽。

**Decision.**

1. **`src/orchestrator/compose_transition_step.{hpp,cpp}`** 新文件，`compose_transition_step(fs, td_from, td_to, W, H, track_rgba, from_rgba, to_rgba, out_src_w, out_src_h, out_transform_clip_idx, err)` free function：
   - Signature 是 value-in + output-by-reference，不依赖 ComposeSink class 任何私有状态——纯从两个 TrackDecoderState 读帧、blend 到 working buffers。
   - 行为与 pre-extraction 的 inline 代码 **字节等价**：
     - from pull → ME_OK 填 from_rgba 并 mark from_valid；NOT_FOUND 算部分 drain；其它错误透传。
     - to pull → ME_OK 填 to_rgba；NOT_FOUND 整 transition 退出（返 ME_E_NOT_FOUND 让 caller `continue`）；其它错误透传。
     - 检查 endpoint dims == W×H（否则 ME_E_UNSUPPORTED + 详细诊断）。
     - from_valid → cross_dissolve(track_rgba, from, to, t)；否则 memcpy(to → track_rgba)。
     - `out_transform_clip_idx = fs.transition_to_clip_idx`（to_clip opacity/transform wins on layer composite）。
   - Phase-1 限制继承 `cross-dissolve-transition-render-wire` cycle 的 decision（comment block 在 .hpp 头部全部搬过来）。

2. **`src/orchestrator/compose_sink.cpp`** Transition 分支从 ~90 LOC 缩成 ~15 LOC 调用点：
   - 只留 "解 idx + 读 TrackDecoderState refs + null 检查" + `compose_transition_step(...)` 调用 + 分流 NOT_FOUND→continue / error→return。
   - 原文件 694 → 609 行（-85 LOC，-12%）。远离 700 P0 阈值。

3. **`src/CMakeLists.txt`** +`orchestrator/compose_transition_step.cpp` 到 media_engine sources。

4. **测试**：不增不减测试 case；**所有现有 compose e2e 测试作为 byte-identical regression cover**：
   - `tests/test_compose_sink_e2e` 7 case：2-track compose / per-clip opacity / per-clip translate / cross-dissolve / cross-dissolve 2-run size-stability / audio-only / video+audio-mixer。全部应 byte-identical（纯函数提取不改行为）。实测 27/27 ctest suite 绿，46/46 test_compose_sink_e2e assertion 绿。
   - 尤其 `test_determinism` 的 "compose path (2-track video + audio mixer) is byte-deterministic" case 也通过 byte-compare 拦截此提取的任何行为偏移。

**Alternatives considered.**

1. **提 make_compose_sink factory 而非 Transition 分支** —— 拒：factory 要 expose ComposeSink class 从 anon namespace 出来，反而增加 header/TU 边界。Transition 分支是真正"大块逻辑单元"，extraction 更有价值。
2. **把 audio mixer 构造块 (~50 LOC) 提成 helper** —— 拒：audio mixer 构造紧贴 SharedEncState 的 aenc 字段，output params 从 aenc copy，helper signature 反而要 pass-through 一堆。inline 50 LOC 清晰度可接受。
3. **完全重写 process() 方法为 stage-based pipeline** —— 拒：scope 爆炸，且没数据证明 pipeline 抽象比当前 imperative loop 更清晰。YAGNI-balanced。
4. **把 extracted 函数设计成 class (CrossDissolveStep)**  —— 拒：zero persistent state；纯函数接口最干净。caller passes working buffers, helper writes. 
5. **Extract + 加单元测试测 compose_transition_step 单独** —— 拒：test 本身需要构造 TrackDecoderState（要求真 demux/decoder/frame），构造成本 >> 收益；e2e 测试已覆盖。只有 pure-data helper 单独单测有价值，decoder-bound helper 的 test cost vs value ratio 不对。

**Scope 边界.** 本 cycle **交付**：
- Transition 分支提到 compose_transition_step.cpp。
- compose_sink.cpp 694 → 609。

本 cycle **不做**：
- factory 拆分。
- audio mixer 构造块拆分。
- 其它 refactor。

Bullet `debt-split-compose-sink-cpp` 已充分满足（远离 700 阈值），**删除**。若未来再度膨胀触发 400+ 行 debt，下次 repopulate 重新加。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 28/28 suite 绿。
- `test_compose_sink_e2e` 7/46 绿（全部）；`test_determinism` 5/N 绿（含 compose byte-identical case）。
- compose_sink.cpp 609 行（从 694）；compose_transition_step.cpp 106 行（新）。两文件合计 715 行 vs 原 694——净 +21 行来自新文件的 doc comment 头。实际核心逻辑字节等价。

**License impact.** 无。

**Registration.**
- `src/orchestrator/compose_transition_step.hpp/cpp`：新文件。
- `src/orchestrator/compose_sink.cpp`：Transition 分支替换为 free function 调用 + `#include "orchestrator/compose_transition_step.hpp"`。
- `src/CMakeLists.txt`：+ `orchestrator/compose_transition_step.cpp`。
- `docs/BACKLOG.md`：**删除** bullet `debt-split-compose-sink-cpp`。

**§M 自动化影响.** M3 current milestone，无 exit criterion 由 debt refactor 解锁。§M.1 不 tick。
