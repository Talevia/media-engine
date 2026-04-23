## 2026-04-23 — cross-dissolve-transition：transitions schema/IR 先行，alpha 混合内核拆出子 bullet（Milestone §M2 · Rubric §5.1）

**Context.** M2 exit criterion "Cross-dissolve transition" 要求在两条 adjacent clip 之间做 alpha 混合渐变。本 cycle 前：

- `docs/TIMELINE_SCHEMA.md:255` 明写 "Transitions between clips on the same track — planned, not parsed"。
- `grep -n 'transition\|dissolve' src/timeline/ src/orchestrator/` 返空——IR 和任何 render 路径都不认识 transition 概念。
- schema doc `line 199` 引用 cross_dissolve 作为独立分类，但没给出具体 JSON 形状。

本 cycle 走 multi-track-video-compose / audio-mix-two-track 同构的 scope A 模式：schema+IR+loader 先行，alpha 混合 + sink 重构 推到 `cross-dissolve-kernel`。三 cycle 三 scope-A 之后 M2 主要子任务（多轨视频、多轨音频、过渡）的 schema/IR plumbing 统一完成，开启 kernel 实装 cycle。

**Decision.** IR + loader + exporter + 9 tests + BACKLOG 重组：

1. **IR 扩展**（`src/timeline/timeline_impl.hpp`）：
   - 新 `enum class TransitionKind : uint8_t { CrossDissolve = 0 }`。ABI-stable append-only。
   - 新 `struct Transition { kind; track_id; from_clip_id; to_clip_id; duration }`——`track_id` 是冗余字段（和两个 clip 的 track_id 相同）用于快速分组。
   - `Timeline::transitions: std::vector<Transition>`——flat 列表，和 `Timeline::clips` 同构（每条用 track_id 分组）。
   - **不**把 transitions 嵌套进 `Track::transitions`——保持 Track 作纯 metadata 的既定设计（与 Clip 一致地在 Timeline 级 flat 存放），让所有"跨 N 个 track 的操作"在 flat list 上 uniform 处理。

2. **Loader 新增 transition 解析**（`src/timeline/timeline_loader.cpp`）：
   - Per-track 增加两个局部状态：`std::vector<std::string> track_clip_ids`（JSON 顺序的 id 列表）+ `std::unordered_map<string, me_rational_t> clip_dur_by_id`。
   - clip 循环中填充这两个结构 + 校验 clip `id` 非空且本 track 内唯一（ME_E_PARSE "duplicate clip id" 若重复——loader 以前从不读 `clip.id`，本 cycle 第一次需要；transition lookup 必须靠这个 id）。
   - clip 循环后读 `track.transitions`（可选数组）。对每个 transition：
     - `kind == "crossDissolve"`：其他值 `ME_E_UNSUPPORTED`（schema v1 phase-1 只支持一种；wipe / slide / dip-to-black 留给 future milestone）。
     - `fromClipId` / `toClipId` 非空，都在本 track 的 clip id 表里。
     - 邻接：用 `track_clip_ids` 线性查找 from 索引，assert `track_clip_ids[from_idx + 1] == to_id`。
     - duration 正（`ME_E_PARSE "duration must be positive"`）；duration ≤ min(from.dur, to.dur)（`ME_E_PARSE "must not exceed either adjacent clip's duration"`）。rational 比较用 `dur.num * cd.den ≤ cd.num * dur.den`（避 floating-point 不确定性）。
   - 通过验证的 Transition 塞进 `tl.transitions`，`kind = CrossDissolve` + track_id 戳入。

3. **Exporter 新 gate**（`src/orchestrator/exporter.cpp`）：
   - 在现有 audio-track / multi-track gate 之后加 `if (!tl_->transitions.empty()) return ME_E_UNSUPPORTED "cross-dissolve / transitions not yet implemented — see cross-dissolve-kernel"`。
   - 单 video track + 有 transitions 的 timeline 是本 cycle 新开放的输入形式，若不在 Exporter 拦截就会走 concat 路径静默产出"hard cut"——比错误 error message 更隐蔽。**Net stubs +1**（新增 gate），带 matching `cross-dissolve-kernel` follow-up，符合 SKILL §3a rule 7 exception (a)。

4. **Tests**（`tests/test_timeline_schema.cpp`）—— 9 个 TEST_CASE append：
   - Happy path：2-clip + 1 crossDissolve 0.5s transition → IR 校验 `transitions.size()==1`，kind / track_id / from / to / duration 都正确。
   - Exporter 拒绝：同样 input → `me_render_start == ME_E_UNSUPPORTED` + err 含 "cross-dissolve / transitions not yet implemented"。
   - Unknown kind（`"wipe"`）→ `ME_E_UNSUPPORTED` + err 含 "only 'crossDissolve'"。
   - Unknown fromClipId（`"c-bogus"`）→ `ME_E_PARSE` + err 含 "fromClipId refers to unknown clip"。
   - 非邻接（3-clip c1→c2→c3，transition c1→c3 跳过 c2）→ `ME_E_PARSE` + err 含 "immediately follow fromClipId"。
   - Duration 超过 adjacent clip（4s > 2s）→ `ME_E_PARSE` + err 含 "must not exceed either adjacent clip"。
   - Duration 为 0 → `ME_E_PARSE` + err 含 "duration must be positive"。
   - Absent transitions 字段 → `transitions.empty()`（backward-compat）。
   - 重复 clip id within track → `ME_E_PARSE` + err 含 "duplicate clip id"（新开的 validation 顺带测）。

5. **Schema doc 不改**——本 cycle 只扩 loader，schema doc 的 "Not yet supported" 段落关于 "Transitions ... planned, not parsed" 的说明本来就正确（compose/mix 内核都还没做；只是 schema plumbing 现在可以 load）。更新成 "schema parsed, render engine rejects until cross-dissolve-kernel" 过于细碎，等实装 cycle 再统一修。

6. **BACKLOG 重组**：
   - 删 `cross-dissolve-transition` bullet。
   - P1 末尾 append `cross-dissolve-kernel` bullet。

**Cross-dissolve 的渲染语义留给后续 cycle 决定**。目前 IR schema 只刻画 "两个相邻 clip 之间 N 秒的 overlap"；到底是 "from-clip 末尾 N 秒 + to-clip 开头 N 秒的 symmetric mix"，还是 "to-clip 开头 N 秒单边 mix"，或是 "需要 sourceRange head/tail handles"——这些是 render-time 的 semantic 决定，到 `cross-dissolve-kernel` 落地时根据实际 demux 能力再定。本 cycle 只保证 IR 能表达 "which two clips, how long"，留有余地。

**Alternatives considered.**

1. **把 transitions 嵌到 Track 里** (`Track::transitions: vector<Transition>`)—— 拒：和上两 cycle 设定的 "Track 是 metadata, 业务数据在 Timeline flat list" 分离原则冲突。保持一致。
2. **不做 adjacency check**（compose kernel 自己用 timeRange 重合判断） —— 拒：schema 层可以 catch 的事情别留给 kernel；且 phase-1 clips 是 strictly contiguous 的，非邻接 transition 数学上无意义。
3. **duration 上限用 (from.dur + to.dur) 而非 min** —— 拒：cross-dissolve 需要同时从两 clip 取 N 秒；min 是业界共识（一个 clip 不可能贡献超出自己长度的内容）。
4. **支持多种 transition kind 一次性**（wipe / slide / fade-to-black） —— 拒：per SKILL §3a 一次一个 enum 值最保守；其他 kind 留到各自的 backlog bullet。
5. **crossDissolve 作 JSON string 值 "cross_dissolve"（蛇形命名）**（匹配 TIMELINE_SCHEMA.md:199 的 `cross_dissolve` 引用）—— 拒：timeline schema 的其他 enum（"bt709", "limited", "static"）都是 camelCase / 单词形式；transition kind 用 camelCase `crossDissolve` 和已有约定一致。schema doc 的 `cross_dissolve` 引用是笔误，留后续 cycle 整理。
6. **用 C enum `ME_TRANSITION_KIND_*` 暴露到 public header** —— 拒：transitions 不进 C API surface（它们是 JSON schema 层的概念，engine 内部用；host 只需要 load JSON 不需要查询 transition 状态）。`enum class me::TransitionKind` 内部可见。
7. **交给一个 transition 专用 subdir `src/timeline/transitions.cpp`** —— 拒：目前只是 IR 字段 + 一段 loader 逻辑，90 行代码没必要独立 TU。真做 kernel 时 `src/compose/transitions/` 是合理拆分。
8. **cross-dissolve-kernel 并入 multi-track-compose-kernel**（"video compose kernel" 把 transition 也包括） —— 拒：transition 合成语义（两个同轨 adjacent clip overlap）和 multi-track compose（多轨同时 overlay）是不同问题；可以共享底层 alpha-over primitive，但每个都需要独立的 edge-case 处理（transition 需要 source handle / timing 对齐；compose 需要 z-order 解析）。保持独立 bullet。

业界共识来源：Premiere / FCP / DaVinci Resolve 的 transition 模型（clip.transition 作 track-level 属性）、OTIO 的 `Transition` schema（`in_offset` / `out_offset` 对称 overlap 设计）、AAF 规范的 `transition` 对象。本项目选择 "duration 作总 overlap 时长，语义推迟到 render-time 决定" 是 schema 上最不过度约束的 shape。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 16/16 suite 绿。
- `build/tests/test_timeline_schema` 46 case / 240 assertion（先前 37/198；+9 case / +42 assertion）。
- 其他 15 个 suite 全绿。single-track 不带 transitions 的 render 路径完全不受影响（`transitions.empty()` 为 true，Exporter gate 不命中）。
- `Clip::id` 在 loader 现在必读——之前 loader 没读它，但 JSON schema 里 `id` 一直是 required 字段，现有测试数据都带了，无破坏性。

**License impact.** 无。

**Registration.**
- `src/timeline/timeline_impl.hpp`：`enum class TransitionKind`、`struct Transition`、`Timeline::transitions`。
- `src/timeline/timeline_loader.cpp`：per-track clip-id 记录、transitions 数组解析（adjacency + duration bound 验证）、clip id 在 track 内唯一性。
- `src/orchestrator/exporter.cpp`：non-empty transitions gate。
- `tests/test_timeline_schema.cpp`：+9 TEST_CASE。
- `docs/BACKLOG.md`：删 `cross-dissolve-transition`，P1 末尾加 `cross-dissolve-kernel`。

**§M 自动化影响.** M2 exit criterion "Cross-dissolve transition" 本 cycle **未完成**——schema/IR 就位但 alpha 混合内核缺。§M.1 evidence check 结果：`src/` 无 transition render 实装，在 `cross-dissolve-kernel` bullet 里；本 exit criterion 保留未打勾。
