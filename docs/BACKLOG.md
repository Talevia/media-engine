# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

---

## P0（必做，阻塞当前 milestone）

- **graph-task-bootstrap** — `src/graph/` / `src/task/` / `src/scheduler/` 只有 README，五模块约定的基础类型都还没 code。**方向：** 按 `docs/ARCHITECTURE_GRAPH.md` 实装：Node/Graph/Builder 纯数据（多 I/O Port + 命名 terminal）、TaskKindId 枚举 + KernelFn 注册表 + TaskContext、Future + EvalInstance + Scheduler 最小执行环路——到"单 Node 的 Graph 能被 scheduler.evaluate_port 求值跑出 kernel 并产出 output"为止。Milestone §M1，Rubric §5.1 + §5.2。
- **timeline-segmentation** — Timeline 按时间段对应多个 Graph 的前提是有切段算法；目前没有。**方向：** `src/timeline/segmentation.{hpp,cpp}` 实装 `Segment` + `segment(Timeline)`；单 clip Timeline → 1 段；两 clip + cross-dissolve Timeline → 3 段（前段独占、过渡段、后段独占）。带 doctest 单测覆盖两种 case。Milestone §M1，Rubric §5.1。
- **refactor-passthrough-into-graph-exporter** — 当前 `src/io/ffmpeg_remux.cpp` 单线程直接跑 remux；要迁到 `io::demux` kernel + `orchestrator::Exporter` passthrough specialization。**方向：** 拆 demux 逻辑为 kernel，mux 逻辑归 Exporter 自持的 `MuxerState`；passthrough 走 stream-copy 特例路径；行为 bit-identical（`01_passthrough` 回归绿）。依赖 `graph-task-bootstrap` 和 `orchestrator-bootstrap`。Milestone §M1，Rubric §5.1 + §5.3。
- **probe-impl** — `me_probe` / `me_media_info_*` 目前全部返回 `ME_E_UNSUPPORTED`。**方向：** 基于 libavformat 实装：open → find_stream_info → 填充 container / codec / W×H / 帧率 / 采样率 / 声道 / duration。Milestone §M1，Rubric §5.1 + §5.2。
- **reencode-h264-videotoolbox** — render 路径目前只支持 `video_codec="passthrough"`。需要第一条真正的 re-encode 路径以解除 M1 的 passthrough 依赖。**方向：** `me_output_spec_t.video_codec="h264"` 走 `h264_videotoolbox`（mac HW encoder，LGPL-clean），音频走 AAC。不引入 GPL 组件。Milestone §M1，Rubric §5.6。
- **test-scaffold-doctest** — 无单元测试框架；靠人工跑 example 验证。**方向：** 加 `ME_BUILD_TESTS=ON` 时链 doctest（FetchContent，MIT），在 `tests/` 建第一批 C API 最小用例（create/destroy、status_str 全覆盖、schema rejection）。Milestone §M1，Rubric §5.2。

## P1（强烈建议，M1 收尾或 M2 起步）

- **engine-owns-resources** — engine 目前没持有 FramePool / CodecPool / Scheduler；生命周期散乱。**方向：** `src/core/engine_impl.hpp` 加字段；`me_engine_create` 同时创建这三者；destroy 时 drain + free。为后续 orchestrator 访问它们奠基。Milestone §M1，Rubric §5.2。
- **taskflow-integration** — `src/scheduler/` 的 CPU pool 要接 Taskflow（MIT）作为 work-stealing executor。**方向：** CMake `FetchContent_Declare` Taskflow v3.7+，在 `src/scheduler/scheduler.cpp` 里用 `tf::Executor` 跑一个 3-节点 DAG smoke 用例，观察多核心占用。同步在 `ARCHITECTURE.md` 依赖表确认 license。Milestone §M1，Rubric §5.5 + §5.6。
- **orchestrator-bootstrap** — Previewer / Exporter / Thumbnailer 只有 README；需要最小骨架能跑。**方向：** 每个 class 构造收 Timeline + engine；内置 `SegmentCache`；提供 `frame_at(t)` / `export_to(spec, cb)` / `png_at(t, ...)` 的最小实装（先委托到现有 passthrough 路径即可，不要求功能完整）；C API `me_render_start` / `me_render_frame` / `me_thumbnail_png` 桥接到对应 orchestrator。依赖 `graph-task-bootstrap` + `engine-owns-resources`。Milestone §M1，Rubric §5.1。
- **thumbnail-impl** — `me_thumbnail_png` stub。**方向：** seek 到目标时刻 → 解码 1 帧 → 按 max_w/h 保比缩放 → PNG 编码。走 libavcodec 软解即可，不走 HW。Milestone §M1，Rubric §5.1。
- **multi-clip-single-track** — loader 强制"exactly one clip"。**方向：** 单轨多 clip concat + 裁剪（sourceRange.start / duration 可非零），输出顺序拼接。仍限制单 track。Milestone §M1，Rubric §5.1。
- **content-hash-asset** — Asset schema 有 `contentHash` 字段但引擎没用。**方向：** 若 JSON 缺 contentHash，首次打开 asset 时流式 sha256 并缓存；若提供则信任。为后续 cache key 做准备。Milestone §M1，Rubric §5.1 + §5.4。
- **determinism-regression-test** — 没有任何测试锁定"软件路径输出字节稳定"。**方向：** doctest 里加：同 JSON 渲染两次，断言两个 MP4 字节相同（passthrough 场景下 trivially 应成立，用它兜底）。Milestone §M1，Rubric §5.3。
- **debt-stub-inventory** — 代码里 stub 散落（cache_*、thumbnail_*、render_frame），没有单一视图看「还有多少 API 没实装」。**方向：** `tools/check_stubs.sh`（grep `ME_E_UNSUPPORTED` 外加白名单），输出未实装函数表。CI / iterate-gap 的 M1 进度可直接读它。Milestone §M1，Rubric §5.2。
- **debt-thread-local-last-error** — `engine_impl.hpp` 目前用 mutex 守 last_error，但 API.md 承诺「thread-local per engine」。**方向：** 换成 `thread_local std::string` slot per engine（用 `std::unordered_map<std::thread::id, string>` 或真正 `thread_local` 变量带 engine 区分），mutex 保留给初始化/销毁。Milestone §M1，Rubric §5.2。
- **debt-update-architecture-md** — `docs/ARCHITECTURE.md` 已加了五模块条目但信息密度偏低。**方向：** 把 "Current implementation state" 表按 graph / task / scheduler / resource / orchestrator 五模块归位重排（目前是"按 C API 函数"组织）；把 Module layout 的五个新目录的 "scaffolded" 状态更到 impl landing 进度。Milestone §M1，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **ocio-integration** — 暂无色彩管理。**方向：** OpenColorIO FetchContent，assets 的 colorSpace → 工作空间转换 → 输出空间。依赖 probe-impl 先落。Milestone §M2，Rubric §5.1。
- **multi-track-video-compose** — 只支持单轨。**方向：** 多 video track 叠加，alpha + blend mode（normal/multiply/screen）。Milestone §M2，Rubric §5.1。
- **audio-mix-two-track** — 音频不合成。**方向：** 2+ audio track 重采样到公共输出率后相加，简单 peak limiter 防爆。Milestone §M2，Rubric §5.1。
- **debt-schema-version-migration-hook** — schema v1 rejection 只认 `== 1`，没有 v2 迁移预演。**方向：** loader 里留 `migrate(v_from, v_to)` 接口，即使只支持 v1 也显式走一遍 migration path，未来 v2 接入零改动。Milestone §M2，Rubric §5.1。
