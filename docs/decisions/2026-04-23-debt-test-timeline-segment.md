## 2026-04-23 — debt-test-timeline-segment：`timeline::segment()` 的 doctest 回归覆盖（Milestone §M1-debt · Rubric §5.2）

**Context.** `me::timeline::segment()`（`src/timeline/segmentation.cpp`）是 IR-到-orchestrator 的边界函数：每个 orchestrator（Previewer / Exporter / CompositionThumbnailer）都走 segment 列表决定 per-segment Graph 该 compile 还是 cache。但 `tests/` 里**没有一条**直接覆盖它——唯一的验证是 `examples/03_timeline_segments` 自己的 hand-rolled CHECK macro + stdout print。那条路径只在手跑 example 时才执行，**不在 CI tripwire 里**。未来谁重构 event-time dedup 或"clip active in segment"谓词（`rat_less` 比较、 clip range 交集）时会无声滑过 CI。

**Decision.** 新 `tests/test_timeline_segment.cpp`，5 个 test case / 32 assertion，复用 `03_timeline_segments` 的 scenario 集合但移植成 doctest shape：

1. **Empty timeline → 0 segments**：contract 为"空 duration 或空 clips 返回空 vector"。
2. **单 clip [0, 2s) 覆盖整个 2s duration → 1 个 segment**：start/end 匹配、active_clips 包含 idx=0。
3. **两 abutting clips [0, 1) + [1, 2) → 2 个 segment**：每个 segment 只 active 一个 clip，boundary_hash 不相等（不同 active set → 不同 hash 的 deterministic 映射）。
4. **Gap-clip-gap（时间线 3s，clip 只在 [1, 2)）→ 3 个 segment**：gap / clip / gap。Gap 段 active_clips 为空；两个 gap 段的 boundary_hash **相等**（Segment-level cache key 的正确性保证：同 active set 的 disjoint segment 共享 compiled Graph）；gap 与 clip 段的 hash 不等。
5. **跨 Timeline 实例的 hash 稳定性**：两个独立构造的 Timeline，Clip 布局完全相同，`segment()` 产的 boundary_hash 相同（纯函数契约）。

不覆盖 overlapping-clip scenario（03_example 里有）：那条路径需要手工构造违反 loader 约束的 IR（overlapping clips），属于专门的 IR-robustness 测试 scope，不在本 cycle 的 doctest 覆盖目标内。

**Test 依赖 `src/` 私有 header.** `segment()` 和 `me::Timeline` 都是 internal types（`src/timeline/segmentation.hpp` + `src/timeline/timeline_impl.hpp`），test 通过 `target_include_directories(test_timeline_segment PRIVATE ${CMAKE_SOURCE_DIR}/src)` 访问——和 `test_timeline_schema` / `test_content_hash` 同一个 pattern。

**Alternatives considered.**

1. **把 `03_timeline_segments` example 改成 test**——拒：example 有独立价值（自包含、可单独跑、print 格式帮人肉 debug）。Test 应该是**并存**的回归哨，不是 example 的替代。
2. **测通过 `me_timeline_load_json` 间接触发 segment**（端到端）——拒：loader 加了过多约束（contiguous clips、不允许 overlap、schemaVersion、colorSpace 必填），掩盖 `segment()` 本身的行为。直接喂 IR 能精准测 segment 算法。
3. **扩 TimelineBuilder 暴露 `Timeline` struct 直接返回**——拒：Builder 设计是生成 JSON，改它的返回类型会波及 7 个现有 suite。`test_timeline_segment` 用 `mk_clip` 本地 helper 更 focused。
4. **把 overlapping-clip scenario 一起覆盖**——拒：本 cycle scope 是"覆盖 loader 允许的正常路径"，overlap 是 loader 反对的 IR shape，应该在 IR-robustness 专门 cycle 里测。

业界共识来源：移植 example → test 是 open-source 生态的标准 coverage 动作（FFmpeg 的 fate suite 大量 case 来自历史 bug reproducer；LLVM 的 test 常常是过去的 crash reducer）。关键是让 CI 能静默地 catch 回归，不依赖人记得跑 example。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 11/11 suite 绿（`test_timeline_segment` 是第 11 个）。
- `build/tests/test_timeline_segment -s`：5 case / 32 assertion / 0 skip / 0 fail。
- 不动 src/，其他 10 个 suite 继续绿。

**License impact.** 无依赖变更。

**Registration.** 无 C API / schema / kernel 变更。CMake 侧：
- `tests/CMakeLists.txt` 的 `_test_suites` 列表尾部加 `test_timeline_segment`。
- `target_include_directories(test_timeline_segment PRIVATE ${CMAKE_SOURCE_DIR}/src)` 让 test 看到 internal headers。
