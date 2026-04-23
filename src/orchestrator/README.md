# src/orchestrator/

**职责**：接 Timeline，按段切分 + 缓存 per-segment Graph，在 Graph 之上组织多帧处理。各自带状态（encoder / muxer / throttle 策略），不污染 Graph。

See `docs/ARCHITECTURE_GRAPH.md` §用户面 API + §批编码 for the contract.

## 当前状态

**Scaffolded, impl incoming**——本目录只有这个 README。具体文件由 backlog `orchestrator-bootstrap` 实装。

## 计划的文件

- `previewer.hpp` / `previewer.cpp` — 单帧 / latency-first；未来加 skip-on-miss
- `exporter.hpp` / `exporter.cpp` — 批编码 / throughput-first；持 `EncoderState / MuxerState`；lookahead pipeline
- `composition_thumbnailer.hpp` / `composition_thumbnailer.cpp` — **composition-level** 单帧 → PNG（timeline + 时间 → 渲染后一帧）；按 `max_w / max_h` 保比缩放。**不是 asset-level thumbnail**——后者是 `src/api/thumbnail.cpp` 里的 C API 直连 demux+decode+encode 管线，不走 orchestrator
- `segment_cache.hpp` / `segment_cache.cpp` — 轻量 `SegmentKey → shared_ptr<Graph>` 缓存；当前 per-orchestrator 持有

## 三者的共同形态

```cpp
class <Orchestrator> {
public:
    <Orchestrator>(me_engine_t*, std::shared_ptr<const timeline::Timeline>);
    Future<T> <action>(…);

private:
    me_engine_t*                              engine_;
    std::shared_ptr<const timeline::Timeline> tl_;
    SegmentCache                              graph_cache_;
};
```

统一模式：`find_segment(t)` → `graph_cache_.get_or_compile(seg)` → `scheduler.evaluate_port(g, terminal, ctx{t})`。

## 边界

- ❌ 不定义 kernel（那是 task module 的事）
- ❌ 不拥有 Node（Node 归 Graph）
- ❌ 不解析 Timeline JSON（timeline module 的事）
- ❌ 不管 GPU / 线程池（scheduler / resource 的事）
- ❌ **不是 editor**——交互编辑是 talevia 宿主的责任，orchestrator 不维护 mutable 状态，也不响应 user events
