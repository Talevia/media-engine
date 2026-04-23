## 2026-04-22 — timeline-segmentation（Milestone §M1 · Rubric §5.1）

**Context.** ARCHITECTURE_GRAPH.md §批编码决定 Orchestrator 持 Timeline 而非 Graph——同一 Timeline 在不同时间段对应不同 Graph（转场过渡期活跃 clip 集变化）。orchestrator-bootstrap 需要一个函数能把 Timeline 切成 Segments，每段内活跃集恒定，对应一次 graph::compile_segment 的输入。这次把该算法实装到位。

**Decision.** `src/timeline/segmentation.{hpp,cpp}`：

- `Segment { start, end, active_clips, active_transitions, boundary_hash }`——纯数据。transitions 字段为未来 IR 扩展预留（当前 Timeline IR 不含 transition 概念）。
- `segment(Timeline)` 算法：
  1. 收集 event times——每个 clip 的 start 和 end，外加 `{0, tl.duration}` 作哨兵
  2. 有理数排序 + 去重（cross-multiply 比较，不转浮点）
  3. 对每对相邻 `(t_i, t_{i+1})`，找出 time range `[t_i, t_{i+1})` 内**完全覆盖**的 clip 集（因为我们在每个 clip 边界切了，部分重叠不可能）
  4. 计算 `boundary_hash = FNV1a(active_clips.idx... | active_transitions.idx...)`——**不含**时间范围本身，所以跨段相同活跃集共享同一 hash（让 orchestrator 的 Graph 缓存天然命中）
- Hash 选 FNV-1a 64-bit 与 graph/graph.cpp 同步，为未来 orchestrator 做 `hash_combine(segment.boundary_hash, output_spec_hash)` 键铺路。

Smoke 验证（`examples/03_timeline_segments/main.cpp`）6 个 case 全过：
1. 空 Timeline → 0 段
2. 单 clip [0,2) → 1 段
3. 两相邻 clip [0,1) [1,2) → 2 段
4. 前后带 gap 的单 clip → 3 段（gap/clip/gap）
5. 重叠 clip [0,2) + [1,3) → 3 段（A / A+B / B）
6. boundary_hash 确定性：相同活跃集的两个不同时间段段 hash 相等；不同活跃集 hash 不等

02_graph_smoke 回归通过。

**Alternatives considered.**

1. **将 transitions 从 IR 剔除 Segment 定义**——"现在没 transition 就别在 Segment 里放 active_transitions 字段"。被拒：字段是免费的（empty vector），现在预留避免未来 transition 接入时改 Segment struct 破坏 orchestrator。
2. **boundary_hash 含时间范围**——这样 "[0,1) clip0" 和 "[2,3) clip0" hash 不同。被拒：orchestrator 对结构相同但时间不同的段，要共享 compiled Graph（time 在 EvalContext 里动态注入，不影响 Graph 结构）；hash 含时间反而降低缓存命中率。
3. **浮点化时间运算**——`double` 转换后用普通比较。被拒：违反 VISION §3.1 有理数时间约束；跨平台浮点差异会让 segment() 输出不可复现。有理数 cross-multiply 虽然略啰嗦但 bootstrap 场景性能无压力。
4. **递归 / sweep line 的更复杂实现**——处理未来 transition 重叠、N^2 clip、时间轴百万 event 的 scale。被拒：当前 clip 数量上限远小于 event 排序成本，简单 O(N log N) 足够；真到瓶颈再做 sweep line。
5. **把 Segment 放在 graph/ 而非 timeline/**——因为 graph::compile_segment 消费它。被拒：Segment 是 Timeline 结构分析的结果（完全由 Timeline 字段推出），和 graph 内部类型（Node/Arena）无关；属于 timeline/ 模块。graph/ 消费它和 graph/ 消费 Timeline 等价。

**Coverage.** `examples/03_timeline_segments/main.cpp` 6 个 case 全过；01_passthrough 和 02_graph_smoke 均无回归。`clang++ -std=c++20 -Wall -Wextra -Werror` 在所有新文件上通过。

**License impact.** 无新依赖。
