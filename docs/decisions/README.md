# Decisions

每次 `iterate-gap` 完成一个 gap，在这里新建**一个文件**归档决策。**不要** append / edit 已有文件——新文件才能被 `ls` / `grep` / 跨文件聚合干净。

## 文件命名

```
docs/decisions/<YYYY-MM-DD>-<slug>.md
```

- 日期用当天（本地即可），与 commit 日期对齐。
- `<slug>` 复用处理的 backlog bullet 的 slug。
- 同日同 slug 冲突 → 加后缀 `-2`、`-3`。

## 模板

```markdown
## YYYY-MM-DD — 短标题（Milestone §Mx · Rubric §5.y）

**Context.** 这个 gap 为何是本轮挑出来的——对应的 rubric 轴 / milestone 状态 / 当前代码里观察到了什么。

**Decision.** 落地了什么。关键类型名、函数名、文件路径。

**Alternatives considered.** 至少两个。每个：做了什么 + 为何被拒。
「业界共识」要具体到名字才算数（如「FFmpeg passthrough 模式的 pkt->pos=-1 约定」、
「nlohmann::json SAX vs DOM」、「bgfx EffectChain compile-to-single-pass 模式」）。

**Coverage.** 哪些测试 / 例子覆盖了这个改动。

**License impact.** 是否引入新依赖？新依赖的 license 是否在 ARCHITECTURE.md 白名单里？
（纯代码改动写「无新依赖」。）

**Registration.** 本轮动了哪些注册点 / 入口点？逐项列出（或写「无注册点变更」）：
- `TaskKindId` / kernel registry 条目（`src/task/`）
- `CodecPool` / `FramePool` / resource factory（`src/resource/`）
- `Orchestrator` factory（Exporter / Previewer / Thumbnailer）
- 新导出的 C API 函数（`include/media_engine/*.h`）
- CMake target / install export / `FetchContent_Declare`
- JSON schema 新字段 / 新 effect kind / 新 codec 名
这一段是未来重构时反向追踪「动了哪些契约」的唯一入口，请完整记录，不要省略。
```

不记 commit 号——`git log` 本身就能按文件名 / 日期查到对应 commit，归档文件里再重复反而会过时。

## 什么不该进这里

- 实现细节（在代码和注释里）
- 运维笔记（在 CLAUDE.md / ARCHITECTURE.md 里）
- TODO 清单（在 BACKLOG.md 里）
- 愿景讨论（在 VISION.md 里）

这里只记**本轮做了什么选择、为什么是这个选择、拒了什么选择**。

## Finding a decision

74+ files and counting. File names are `<date>-<slug>.md`; finding "why X was done like Y" by filename alone requires knowing the slug convention, which new contributors don't yet. This section lists the search patterns that actually surface answers.

### By module / topic

Each cycle tags the area it touched. Grep from repo root:

```bash
# Timeline IR / schema / loader
grep -l 'timeline/\|Timeline::' docs/decisions/*.md

# Orchestrator (Exporter / Previewer / CompositionThumbnailer / output sinks)
grep -l 'orchestrator/\|Exporter\|OutputSink\|Previewer' docs/decisions/*.md

# Render / reencode path (passthrough + h264/aac)
grep -l 'reencode\|passthrough\|MuxContext\|DemuxContext\|muxer_state' docs/decisions/*.md

# I/O layer (FFmpeg RAII wrappers, av_err_str, demux / mux plumbing)
grep -l 'src/io/\|io/mux\|io/demux\|av_err_str' docs/decisions/*.md

# Graph / task / scheduler / resource (execution model)
grep -l 'graph/\|task/\|scheduler/\|resource/' docs/decisions/*.md

# Color pipeline / OCIO
grep -l 'color/\|OcioPipeline\|IdentityPipeline\|me::ColorSpace' docs/decisions/*.md

# Cache (asset hash / frame / codec pool observability)
grep -l 'AssetHashCache\|FramePool\|CodecPool\|me_cache_' docs/decisions/*.md

# Public C API surface (include/media_engine/*.h contracts)
grep -l 'include/media_engine/\|extern "C"\|me_engine_\|me_render_\|me_timeline_' docs/decisions/*.md

# Build / CMake / FetchContent / fixture
grep -l 'CMakeLists\|FetchContent\|ME_WITH_\|ME_BUILD_\|gen_fixture' docs/decisions/*.md

# Tests / doctest framework / fixture patterns
grep -l 'tests/test_\|doctest\|TEST_CASE\|TimelineBuilder' docs/decisions/*.md

# Docs (README / ARCHITECTURE / VISION / CLAUDE / PAIN_POINTS edits)
grep -l 'docs/VISION\|docs/ARCHITECTURE\|docs/API\|CLAUDE.md\|PAIN_POINTS\|MILESTONES' docs/decisions/*.md
```

### By rubric axis (VISION §5)

Every decision header names its rubric axis — `(Milestone §M<x> · Rubric §5.<y>)`. Grep:

```bash
# §5.1 — Correctness / domain fidelity (schema, IR, render math, transitions)
grep -l 'Rubric §5.1' docs/decisions/*.md

# §5.2 — Developer experience / testability / error surfaces
grep -l 'Rubric §5.2' docs/decisions/*.md

# §5.3 — Performance / determinism / resource use
grep -l 'Rubric §5.3' docs/decisions/*.md

# §5.4 – §5.7 — extend per the VISION rubric table
grep -l 'Rubric §5.4' docs/decisions/*.md
```

### By milestone

```bash
# Everything stamped for the current / past milestone (M1, M2, ...)
grep -l 'Milestone §M1' docs/decisions/*.md
grep -l 'Milestone §M2' docs/decisions/*.md
```

Use `ls docs/decisions | sort -r | head -20` for recent cycles without opening the files.

### By decision keyword

For "did we consider X?" or "why did we reject X?" queries, `grep` the body — every decision doc has an `**Alternatives considered.**` block listing what was rejected and why:

```bash
grep -l -F 'Alternatives considered' docs/decisions/*.md | xargs grep -H -F '<keyword>'
```

Practical examples:

```bash
# Why did we pick VideoToolbox over x264?
grep -l -F 'x264' docs/decisions/*.md
# Why is the task scheduler Taskflow rather than raw threads?
grep -l -F 'Taskflow' docs/decisions/*.md
# Was there a reason not to use nlohmann::json SAX mode?
grep -l -F 'SAX' docs/decisions/*.md
```

### Common question → starting file

| Question | Start here |
|---|---|
| How does the engine know which codec to use? | `2026-04-23-me-render-output-format-infer.md` + `2026-04-22-reencode-h264-videotoolbox.md` |
| Why is the timeline schema shaped the way it is? | `docs/TIMELINE_SCHEMA.md`, then `grep Rubric §5.1` under decisions |
| What's the graph / task / scheduler split for? | `2026-04-22-architecture-graph.md` + `2026-04-22-graph-task-bootstrap.md` + `docs/ARCHITECTURE_GRAPH.md` |
| Why is there an IdentityPipeline? | `2026-04-23-ocio-integration-skeleton.md` → `2026-04-23-ocio-pipeline-enable.md` |
| What counts as a stub vs a real impl? | `CLAUDE.md` "Known incomplete" + `tools/check_stubs.sh` |
| Why is multi-track / cross-dissolve rejected right now? | `2026-04-23-multi-track-video-compose.md` + `2026-04-23-cross-dissolve-transition.md` + their `-kernel` follow-up bullets in `BACKLOG.md` |

### If all else fails

Open `docs/decisions/` in an editor and rely on full-text search across the directory. The "find a decision" UX bottoms out there; the grep patterns above get you to the right ~5 files faster than a filename scan.
