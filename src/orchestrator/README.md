# src/orchestrator/

**职责**：接 Timeline，按段切分 + 缓存 per-segment Graph，在 Graph 之上组织多帧处理。各自带状态（encoder / muxer / 时钟），不污染 Graph。

See `docs/ARCHITECTURE_GRAPH.md` §三种执行模型 + §用户面 API for the contract.

## 当前形状（按执行模型）

| 路径 | 文件 | 用途 |
|---|---|---|
| (a) per-frame | `compose_frame.{hpp,cpp}` | 自由函数 `resolve_active_clip_at` / `compile_frame_graph` / `compose_frame_at` / `compose_png_at`。无类、无状态。 |
| (b) streaming | `exporter.{hpp,cpp}` + `output_sink.*` + `compose_sink.*` + `audio_only_sink.*` + `muxer_state.*` + `reencode_*` | 批编码 / passthrough / re-encode；持 EncoderState + MuxerState 跨帧。 |
| (c) playback session | `player.{hpp,cpp}` + `playback_clock.{hpp,cpp}` + `video_frame_ring.{hpp,cpp}` | 组合 (a)+(b)+ 内部时钟；有 producer / pacer / audio_producer 三线程。 |
| 共享 | `segment_cache.{hpp,cpp}` | `SegmentKey → shared_ptr<Graph>` 缓存。 |

## 路径 (a) 是自由函数，不是类

历史上有过 `Previewer` 与 `CompositionThumbnailer` 两个类承载这条路径。它们都只有一个方法、零跨调用状态——本质是带 `(engine*, tl)` 两个成员的函数。删掉后剩下的 `compose_frame.{hpp,cpp}` 就是这条路径的全部。

调用者：
- `me_render_frame` (`src/api/render.cpp`) — 单帧 RGBA + DiskCache
- `Player::producer_loop` — 实时播放，inline 调 `resolve_active_clip_at` + `compile_frame_graph`，不走 `compose_frame_at` 因为需要保留 in-flight Future 给 seek 取消
- `compose_png_at` 自己 — 内部调 `compose_frame_at` + libsws scale + libavcodec PNG encode

未来要做多轨 compose（backlog `previewer-multi-track-compose-graph`），改 `compile_frame_graph` 一处，三家都受益。

## 边界

- ❌ 不定义 kernel（那是 task module 的事）
- ❌ 不拥有 Node（Node 归 Graph）
- ❌ 不解析 Timeline JSON（timeline module 的事）
- ❌ 不管 GPU / 线程池（scheduler / resource 的事）
- ❌ **不是 editor**——交互编辑是 talevia 宿主的责任，orchestrator 不维护 mutable 状态，也不响应 user events
- ❌ Player **不画 surface**——窗口 / widget / 绘制目标在宿主；引擎只承担"此刻该是哪一帧 / 哪段音频"
