## 2026-04-23 — milestone-advance-m2：Current 从 M1 → M2，seed M2 bootstrap backlog

**Context.** M1 "API surface wired" 的 10 条 exit criteria 已全部达成（见前一个 commit `673941c docs(milestone): tick M1 exit criteria ...`）。同日上游 commit `5847ce2 chore(skill): iterate-gap — milestone auto-tick + auto-advance` 把 milestone 推进从"用户手工改 MILESTONES.md"改为"iterate-gap skill §M 自动处理"。按新规则跑 M.2：全绿 → advance。

**M1 landing evidence 清单**（已 tick 的 10 条，按 MILESTONES.md 顺序）：

| # | Criterion | Landing commit |
|---|---|---|
| 1 | `me_engine_create` / `me_engine_destroy` | bootstrap（早期，M1 启动即可用） |
| 2 | `me_timeline_load_json` schema v1 | bootstrap |
| 3 | `me_render_start` passthrough | bootstrap + `a0435df feat(tests): determinism-regression-test` 验证 |
| 4 | `me_probe` container/codec/duration/W×H/fps/sample_rate/channels | `49ed302` + `8396ae2 me-probe-more-fields` |
| 5 | `me_thumbnail_png` | `156576d feat(thumbnail): thumbnail-impl` |
| 6 | `me_render_start` reencode (h264 via VideoToolbox) | `f9404f0 reencode-h264-videotoolbox` + `2bfa6cd reencode-multi-clip` + `4ad072e debt-render-bitexact-flags` |
| 7 | `me_timeline_load_json` N-clip single track | `f1e290b multi-clip-single-track` |
| 8 | doctest framework + ≥ 1 passthrough determinism regression | `fd5926d test-scaffold-doctest` + `a0435df determinism-regression-test`；现 17 个 suite / 16 ctest 绿 |
| 9 | `me_cache_stats` real counts | `a4a1c1c cache-stats-impl` |
| 10 | graph/task/scheduler/resource/orchestrator 5 模块骨架 + timeline segmentation + passthrough migrated | `268c346 docs(architecture)` + `2045f43 graph-task-bootstrap` + `00a4459 taskflow-integration` + `8951ea2 engine-owns-resources` + `51f4963 orchestrator-bootstrap` + `6fb4be3 timeline-segmentation` + `f5dfa39 refactor-passthrough-into-graph-exporter` |

证据详表在 `docs/M1_AUDIT.md`（本 commit 同步删除——其全部作用就是驱动本次 advance 判断，现在使命完成）。

**Decision.** 按新 iterate-gap §M.2 执行：

1. **Current 指针从 "M1 — API surface wired" → "M2 — Multi-track CPU compose + color management"**（`docs/MILESTONES.md:5`）。
2. **Seed M2 bootstrap backlog**（`docs/BACKLOG.md` P1 档）：
   - **Promote 3 条原 P2 / §M2 bullet 到 P1**（整条移动，slug / Gap / 方向文字不变）：
     - `multi-track-video-compose`（M2 exit: 2+ video tracks 叠加）
     - `audio-mix-two-track`（M2 exit: 2+ audio tracks 混音）
     - `cross-dissolve-transition`（M2 exit: Cross-dissolve transition）
   - **新写 1 条 P1 bullet** 覆盖至今 BACKLOG 里完全没对应项的 M2 exit criterion：
     - `ocio-pipeline-enable`（M2 exit: OpenColorIO 集成）—— `src/color/pipeline.hpp:74` 的 `make_pipeline()` factory 有 `#if ME_HAS_OCIO` 分支但 dead code（OCIO FetchContent 早先因 nested yaml-cpp CMake policy 阻断）。方向：重试 FetchContent 或改走 find_package，翻 `ME_WITH_OCIO` 默认 ON，实装 `OcioPipeline` 类。
   - **保留原 P1** 的 `transform-static-schema-only`（M2 exit: Transform 静态；已在 P1 就位，只把 tag 从 `§M2-prep` 修正为 `§M2`——M2 已成为 current，不再是 "prep"）。
   - **保留原 P1** 的两条 M2-prep 收口 debt：`debt-examples-cmake-macro-tests`、`docs-decisions-dir-readme`——继续有效。
3. **P2 剩余项** 都是 M3-prep / M4-prep / M6-prep，不动。
4. **删除 `docs/M1_AUDIT.md`**（其产出的 tick-off 已完成；归档保留在 `git log --follow docs/M1_AUDIT.md`）。
5. **P1 档头 note 从 "M1 收尾或 M2 起步" → "M2 主线 / 跨 milestone debt"**，反映新的 current。

**M2 exit criteria 的 BACKLOG 覆盖率校验**（§M.2 要求）：

| M2 exit criterion | BACKLOG bullet | 档 |
|---|---|---|
| 2+ video tracks 叠加 | multi-track-video-compose | P1 ✓ |
| 2+ audio tracks 混音 | audio-mix-two-track | P1 ✓ |
| OpenColorIO 集成 | ocio-pipeline-enable | P1 ✓（新增） |
| Transform 静态 e2e | transform-static-schema-only（schema only，compose 要后续扩） | P1 ✓ |
| Cross-dissolve transition | cross-dissolve-transition | P1 ✓ |
| 软件路径 byte-identical 确定性回归 | **已完成**（test_determinism 覆盖 passthrough + reencode；`det-regression-software-path` 是 M1 criterion #3，等 M2 compose 落地自动 extend 到 compose 路径） | — |

6 条 exit criterion，5 条有 bullet（1 条 shared with transform schema-first approach），1 条已事实落地（软件确定性回归走 test_determinism 已有覆盖；M2 compose 实装时延用同 pattern）。

**§M.3 触发检测.** 推进后 P1 = 7 条 bullet（≥ 3）→ 不触发本 cycle 强制 repopulate。下次 /iterate-gap 第 2 步会按 P0→P1→P2 顺序直接挑 P1 top `transform-static-schema-only` 或 `multi-track-video-compose`（排在前面的是 `transform-static-schema-only`，因为它之前就在 P1）。

**Alternatives considered.**

1. **推进但不 tick M1 #8 / #10**（doctest 框架 / 5 模块骨架 —— 这类"松散"criterion 可能被质疑是否 really done） —— 拒：evidence 都在；#8 的 16 个 suite + ctest 全绿，#10 的 5 个模块子目录 + `examples/02_graph_smoke` + `examples/03_timeline_segments` 各自演示端到端。保守派作法是要求严格 "≥ 3 test cases per module"之类度量，但 MILESTONES.md 的 criterion 文字不要求这样的度量，evidence 已足够。
2. **先停一拍让用户审 M1 tick，再推进 M2** —— 拒：iterate-gap §M.2 规则就是全绿直接推。把两步拆开无收益。旧的 `docs/M1_AUDIT.md` 就是为这次 advance 而写的审计 snapshot，作用已尽。
3. **P1 放更多 bullet（例如拆 ocio-pipeline-enable 为 3 步：FetchContent fix → OcioPipeline 实装 → 线程到 sink）** —— 拒：单条 bullet 覆盖 exit criterion 更清晰；真开始做的 cycle 可以自己拆子任务写多个 decision。
4. **P1 头 note 写成 "M2 必做 + M2-prep 收口 debt"** —— 拒："主线 / 跨 milestone debt" 更通用，未来 M3 advance 时不用再改这行的 milestone 号。
5. **把 P2 里的 M4-prep / M6-prep bullet 删掉** —— 拒：它们是未来 milestone 的有效预案记录，留着不损害 current milestone 资源分配。
6. **把 `transform-static-schema-only` 的 Milestone tag 保留 `§M2-prep`** —— 拒：M2 已经是 current，仍然用 "-prep" tag 会让 §M 自动化脚本语义混乱（`-prep` 传统上表示"下一个 milestone 的预启动"，不应用在 current）。改 `§M2` 更干净。M2-prep 继续用于 `debt-examples-cmake-macro-tests` / `docs-decisions-dir-readme` 这两条——它们不对应 exit criterion，只是 current milestone 周期里的 cleanup，tag 用 `§M2-prep` 仍合适。

业界共识来源：semantic milestone gating 在 open-source project 里的典型模式（Rust 的 "MILESTONE achieved" PR merge、CPython 的 PEP lifecycle、Linux kernel 的 merge-window close）——evidence-based auto-gate 比 manual 更稳定。

**Coverage.** 本 commit 不改代码、不跑 test（§M 章节自定义——milestone sync 只动 MILESTONES/BACKLOG/decision/审计文件）。但确认推进前的 ctest 状态（紧跟前一 cycle `1db1f2a`）是 16/16 绿，就是 M1 deliverable 的完整 snapshot。

**License impact.** 无。

**Registration.** 无 C API / schema / kernel 变更。
- `docs/MILESTONES.md:5` Current 指针改 M1 → M2。
- `docs/BACKLOG.md` P1 / P2 重新分档（P2 三条 §M2 item 提升到 P1 + 新增 ocio-pipeline-enable + P1 档头 note 改写 + transform-static-schema-only tag 小修）。
- `docs/decisions/2026-04-23-milestone-advance-m2.md` 新文件。
- `docs/M1_AUDIT.md` 删除。
