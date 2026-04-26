# Graph architecture — the five-module execution model

The runtime engine inside media-engine is organized around five modules with clean boundaries:

```
         Timeline JSON
              │
              ▼
    timeline::Timeline (IR)
              │
              │  + timeline::segment() —— 按时间切成段
              ▼
              ┌─────── orchestrator/ ───────┐
              │ Player(Timeline)            │  ← 播放会话，A/V sync
              │ Previewer(Timeline)         │  ← 单帧，latency-first
              │ Exporter(Timeline)          │  ← 批编码，throughput-first
              │ CompositionThumbnailer(Tl)  │  ← 单帧 PNG（合成后）
              │                             │
              │ 内部：segment → compile_segment
              │        缓存 per-segment Graph │
              │                             │
              │ 每帧：scheduler.evaluate_port
              │       (graph, terminal, ctx{t})
              └──────────┬──────────────────┘
                         │
                         ▼
              graph::Graph (纯数据)
              —— 某一段时间内的节点连接描述
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│                     scheduler::Scheduler                        │
│   对 Graph 每个 Node 产出 Task（运行时短命对象）:                 │
│     Task{ node_ref, kernel_fn（从 registry 查）, props, inputs }│
│   按 Node.affinity 入 CPU / GPU / HwEnc / IO 池                 │
│   Task 执行时 scheduler 把 FramePool / CodecPool / GpuCtx /     │
│   time / cancel 注入 TaskContext，调                            │
│   kernel(ctx, props, inputs, outputs)                           │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
             resource::FrameHandle (refcounted)
                         │
                         ▼
    resource::FramePool / CodecPool / GpuCtx / Budget
```

一句话：**Timeline 描述整个工程，Orchestrator 把它按时间段切成多个 Graph；Graph 和 Node 是纯数据（描述"某段时间内算什么"），Kernel 是 task 模块注册的纯函数（实现"怎么算"），Task 是 scheduler 在执行瞬间物化的 `(Node, EvalContext)` 对**。

Read `docs/VISION.md` first if you haven't. This file is *how the engine actually executes*; VISION is *why*.

## 三种执行模型

本文件描述的 five-module 架构（graph / task / scheduler / resource / orchestrator）只是 media-engine 里**三种**执行模型中的一种。实际代码库里**三种并存**，选哪条路径由 output 的**状态性（statefulness）+ 是否承担时钟**决定。不区分清楚会让新 orchestrator 作者把 encoder lifecycle 塞进 Node kernel，破坏纯函数约束。

### (a) Graph per-frame + scheduler —— stateless per-frame

用于 **preview / thumbnail / frame-cache / future `me_render_frame`**——"给定 time，产一帧"的场景。

- Orchestrator（Previewer / CompositionThumbnailer）每次 `frame_at(t)` 从 Timeline compile 出 per-segment Graph（cached），喂 `EvalContext{time: t}` 给 scheduler。
- Scheduler 对 Graph 的每个 Node 物化一个 Task，执行完即回收；调度顺序不影响输出（VISION §3.1 确定性）。
- Kernel 是纯函数、**不持跨帧状态**。所有 frame-to-frame 依赖走 `resource::FrameHandle` 引用。
- **判别特征**：每次调用**独立**；不要求上次调用留下的 decoder / encoder 状态。
- 适用场景：低延迟单帧、scrubbing、cache lookup、重绘。

本文件下面 §关键理念 / §Node / §Kernel / §scheduler 各节描述的就是这条路径。M1 的 `02_graph_smoke` 走这条。

### (b) Orchestrator streaming —— stateful per-job

用于 **export / re-encode / passthrough concat**——"给定 Timeline，产一整个输出文件"的场景。

- Orchestrator（Exporter → OutputSink → `reencode_mux` / `passthrough_mux`）拿着 `std::vector<DemuxContext>` 和 `OutputSpec`，**开一次** encoder + muxer，**自持** decode → encode → mux 的流式循环。
- 状态**跨帧 / 跨 clip / 跨 segment**：encoder GOP 结构、AAC priming samples、`MuxContext` DTS 连续性、`next_video_pts` / `next_audio_pts` 累加、`AVAudioFifo` 缓冲——都是一次 allocate、多次 reuse。
- 不经过 Graph / Task / scheduler 的 per-frame 循环——若把 "open encoder" / "flush encoder" 塞进 Node kernel，kernel 就不再纯函数，违反关键理念 #3 #4。
- **判别特征**：encoder / muxer / FIFO 状态必须**活过多次帧 / 段调用**。
- 适用场景：完整 MP4 export、passthrough concat、h264/aac re-encode、带 AAC priming 的 multi-clip。

M1 的 `01_passthrough` / `05_reencode` 走这条。主实现在 `src/orchestrator/muxer_state.cpp`（passthrough）+ `src/orchestrator/reencode_pipeline.cpp` / `reencode_audio.cpp` / `reencode_video.cpp`（re-encode）。

### (c) Orchestrator playback session —— 组合 (a) + (b) + 时钟

用于 **timeline preview player**——"持有 Timeline 与播放头，对外承担 play/pause/seek 状态机与 A/V sync，把每一刻该显示的视频帧 + 该播放的音频 chunk 通过 callback 推给宿主"。

- Player（`src/orchestrator/player.cpp`）持 `(current_time, rate, is_playing)` 状态；起 Producer + Pacer 两条线程：Producer 用 (a) 路径每帧 `evaluate_port` 取 video，用 (b) 路径调 `AudioMixer::pull_next_mixed_frame` 拉混音 chunk；Pacer 按 master clock 决定何时调 callback。
- **Master clock**：timeline 含音频时默认 AUDIO（宿主在自己的音频设备 callback 里把当前 playhead 通过 `me_player_report_audio_playhead` 注入），否则 WALL（`std::chrono::steady_clock`）；`ME_CLOCK_EXTERNAL` 预留。
- **A/V sync**："video chases audio"——video frame 的 timeline 时间与 master clock 比较，差 `> +½ frame_period` 等待、`< -½ frame_period` 丢弃、否则 present-now。漂移上限沿用 M4 export 路径的 < 1 ms / 小时（见 `docs/MILESTONES.md`）。
- **不画 UI surface**——宿主负责把 callback 收到的 frame / audio chunk 上屏 / 喂音频设备；Player 只承担"此刻该是哪一帧 / 哪段音频"。这是和 VISION §4 的边界：会话语义在引擎内、绘制目标在宿主。
- **判别特征**：必须**维持播放头状态 + 跨调用时钟**，且对外承担 A/V sync。export 不走这条（export 不需要"此刻"——它要的是"下一帧"，那是 (b)）。
- (b) 路径的 `AudioMixer + AudioTrackFeed` 跨调用样本累计 + per-track FIFO 状态正是 Player 需要的——所以 Player 直接复用 ComposeSink 用的 `build_audio_mixer_for_timeline`，不重写。

主实现在 `src/orchestrator/player.cpp` + `playback_clock.cpp` + `video_frame_ring.cpp` + `audio_chunk_queue.cpp`；C API 在 `include/media_engine/player.h`。

### 何时选哪种

| 输出性质 | per-frame (a) | streaming (b) | playback (c) |
|---|---|---|---|
| 单帧 PNG / thumbnail | ✅ | ❌ | ❌ |
| 完整 MP4 export | ❌（encoder state 无处放） | ✅ | ❌ |
| Scrubbing（host-paced）/ `me_render_frame` 单帧拉取 | ✅ | ❌ | ❌ |
| Multi-clip concat / reencode | ❌ | ✅ | ❌ |
| GPU effect chain on single frame | ✅ | ❌ | ❌ |
| Audio mixing over a timeline | ❌ | ✅ | ❌ |
| **Timeline 预览播放（play/pause/seek + A/V sync）** | ❌（无时钟） | ❌（不知何时呈现） | ✅ |

一句话判别：
- **要不要跨调用持有 encoder / muxer / FIFO / 累积样本计数？** 有 → (b) streaming；无 → (a) per-frame。
- **要不要承担"现在该呈现哪一帧 / 哪段音频"？** 要 → (c) playback，组合 (a)+(b)+ 内部时钟；不要 → 只在 (a)/(b) 里选。

越界（例如想在 Previewer 里流式保留 encoder、或想让 (b) 自己决定播放节拍）→ 停下来重想；不是 path 选错就是 feature 设计错。

三条路径**共用** `resource::FramePool / CodecPool / AssetHashCache`（资源层不区分消费者）和 `timeline::Timeline / timeline::segment()`（IR 是三条路径的共同输入）。分歧在：状态如何跨调用持续、Task 是否被 scheduler 物化、以及是否承担时钟。(c) 不是 (a) 与 (b) 之外的第四种执行原语，而是把两者**组合**起来再加一层 master clock + state machine——所以 (a) (b) 的纯函数约束 / streaming 状态约束在 (c) 内部依然成立。

## 关键理念（本文件的不可让步点）

1. **Node / Graph 纯数据**——`{kind, properties, input ports, output ports, output types}`；没有 `execute()` 方法、不含函数指针、无 vtable；完全可序列化、可哈希、可 diff。
2. **多输入多输出是 Node 的一等特性**——Port 类型化 + 命名。`demux` 的 outputs 是 `{video_packets, audio_packets, metadata}`；`compose` 的 inputs 是 N 个 layer。
3. **Kernel 独立注册**——按 `TaskKindId` 在 `task::registry` 里查；同一 kind 可有多 variant（CPU / GPU / HW），scheduler 按 affinity 挑。
4. **Task 是短命运行时对象**——由 scheduler 在调度某个 Node 的某次求值时 new 出来；所需状态和资源由 `TaskContext` **动态注入**，执行完即丢。
5. **Graph 只描述单段单帧**——时间是 `EvalContext.time`，不是 Graph 状态；Timeline 按时间切分成 Segments，每段 compile 出一个 Graph。Player（执行模型 c）把多次单帧 evaluate 拼成一条播放流，但每一次单独的 evaluate 仍满足这条——播放头 / 时钟住在 Player，不会渗进 Graph。
6. **Orchestrator 持 Timeline**——按需 compile + 缓存 per-segment Graph；各自带状态（encoder / muxer / throttle），不污染 Graph。
7. **子图复用通过 builder helpers**——函数组合形式，compile 后 Graph 天然扁平；**没有 "Graph 嵌套 Graph" 运行时类型**。
8. **Future 是 lazy、scheduler-driven**——由 `scheduler.evaluate_port(graph, terminal, ctx)` 返回；`await()` 时才触发 Task 派发。不是 `std::future` 的 eager 语义。

## 确定性（VISION §3.1 在架构层的体现）

- Graph 和 Node 都是 immutable pure data；`content_hash = xxh(kind || props_blob || inputs.content_hash)` 稳定
- Kernel 是纯函数（相同 inputs + props + time → 相同 outputs）
- Task 只是 `(Node, EvalContext)` 的临时物化，调度顺序不影响输出
- Segment 边界由 Timeline 结构确定性决定（相同 Timeline → 相同 segments → 相同 Graphs）
- HW encoder 路径显式标 "not byte-identical"，回归测试用 SW 路径兜底

---

## 用户面 API

### 外层（C API 封装层用）

```cpp
namespace me::orchestrator {

class Previewer {                                        // (a) 单帧拉取
public:
    Previewer(me_engine_t*, std::shared_ptr<const timeline::Timeline>);
    Future<resource::FrameHandle> frame_at(me_rational_t time);
};

class Exporter {                                         // (b) 流式编码
public:
    Exporter(me_engine_t*, std::shared_ptr<const timeline::Timeline>);
    Future<void> export_to(const OutputSpec&, ProgressCb);
};

class CompositionThumbnailer {                            // (a) 单帧 PNG
public:
    CompositionThumbnailer(me_engine_t*, std::shared_ptr<const timeline::Timeline>);
    Future<Bytes> png_at(me_rational_t time, int max_w, int max_h);
};

class Player {                                            // (c) 播放会话
public:
    Player(me_engine_t*, std::shared_ptr<const timeline::Timeline>, PlayerConfig);
    void play(float rate);                                // 本期仅支持 1.0
    void pause();
    void seek(me_rational_t time);
    void report_audio_playhead(me_rational_t t);          // AUDIO master clock
    void set_video_callback(VideoCb, void* user);
    void set_audio_callback(AudioCb, void* user);
};
}
```

四个都接收 Timeline（不是 Graph）——同一 Timeline 按时间对应多个 Graph，orchestrator 按需 compile + 缓存。Player 与 Previewer 共用 `compile_frame_graph` 但状态模型不同：Previewer 无状态、调用即返一帧；Player 有状态（current_time / rate / is_playing）、长寿命、对外承担时钟。

### 中层（Timeline 切段 + Graph 构建）

```cpp
namespace me::timeline {

struct Segment {
    me_rational_t                start;   // inclusive
    me_rational_t                end;     // exclusive
    std::vector<ClipRef>         active_clips;
    std::vector<TransitionRef>   active_transitions;
    uint64_t                     boundary_hash;  // Graph 缓存键
};

std::vector<Segment> segment(const Timeline&);
}

namespace me::graph {

struct NodeRef { uint32_t node_id; };
struct PortRef { uint32_t node_id; uint8_t port_idx; };

class Graph {
public:
    class Builder {
    public:
        NodeRef add(task::TaskKindId,
                    Properties,
                    std::span<const PortRef> inputs);
        Graph build() &&;
    };

    std::span<const Node>         nodes() const;
    std::span<const NodeId>       topo_order() const;
    uint64_t                      content_hash() const;

    // 多个命名 terminal（e.g. "video" + "audio"）
    std::span<const std::string_view>  terminal_names() const;
    std::optional<PortRef>             terminal(std::string_view name) const;
};

// 子图复用：builder helper 函数，返 output PortRef
PortRef color_correct_fragment(Graph::Builder&, PortRef rgb_in, /* params */);

// Factory：Timeline 的一段 → Graph
me_status_t compile_segment(const timeline::Timeline&,
                            const timeline::Segment&,
                            const CompileOptions&,
                            std::shared_ptr<Graph>* out,
                            std::string* err);
}
```

### Future

```cpp
template<typename T>
class Future {
public:
    bool ready() const;
    T    await();
    template<typename F> auto then(F&&);
    void cancel();

private:
    std::shared_ptr<sched::EvalInstance> eval_;
    uint32_t                             slot_;
};
```

---

## Graph 内部：纯数据

```cpp
namespace me::graph {

struct InputPort {
    std::string_view name;        // "video_in", "mask", "gain", …
    TypeId           type;        // FrameHandle | AudioBuf | Scalar | …
    PortRef          source;      // 指向上游 { node_id, output_port_idx }
};

struct OutputPort {
    std::string_view name;        // "frame", "luminance", …
    TypeId           type;
};

struct Node {
    task::TaskKindId        kind;
    Properties              props;           // typed blob
    std::vector<InputPort>  inputs;          // 多输入
    std::vector<OutputPort> outputs;         // 多输出
    uint64_t                content_hash;    // 建图时算好
    bool                    time_invariant;  // 由 kind 注册时声明
};

class Graph {
    std::vector<Node>                              nodes_;
    std::vector<std::vector<NodeId>>               dependents_;
    std::unordered_map<std::string_view, PortRef>  terminals_;  // "video" → PortRef
    uint64_t                                       content_hash_;
};
}
```

`content_hash` 递归计算：
```
node_hash(N) = xxh(N.kind || N.props || input_port[i].source.content_hash for each i)
graph_hash  = xxh(all nodes in topo order)
```

### 多 I/O Node 示例

```
io::demux
  inputs:  [uri: string (property), start_offset: rational (property)]
  outputs: [video_packets: PacketStream, audio_packets: PacketStream, metadata: MediaInfo]

render::cross_dissolve
  inputs:  [frame_a: FrameHandle, frame_b: FrameHandle, mix: Scalar]
  outputs: [frame: FrameHandle]

render::compose
  inputs:  [layer_0..layer_N: FrameHandle]
  outputs: [frame: FrameHandle]

algo::analyze
  inputs:  [frame: FrameHandle]
  outputs: [frame: FrameHandle (passthrough), histogram: HistBlob, motion: MotionVectors]
```

---

## Task 运行时与 Kernel 注册

### task 模块

```cpp
namespace me::task {

enum class Affinity : uint8_t { Cpu, Gpu, HwDecoder, HwEncoder, Io };
enum class Latency  : uint8_t { Short, Medium, Long };

struct TaskContext {
    me_rational_t            time;
    resource::FramePool*     frames;
    resource::CodecPool*     codecs;
    resource::GpuContext*    gpu;         // null if CPU kernel
    const std::atomic<bool>& cancel;
    Cache*                   cache;
};

using KernelFn = me_status_t (*)(TaskContext& ctx,
                                 const Properties& props,
                                 std::span<const InputValue> inputs,
                                 std::span<OutputSlot> outputs);

struct KindInfo {
    TaskKindId                 kind;
    Affinity                   affinity;
    Latency                    latency;
    bool                       time_invariant;
    KernelFn                   kernel;
    std::span<const PortDecl>  input_schema;
    std::span<const PortDecl>  output_schema;
    std::span<const ParamDecl> param_schema;
};

void register_kind(const KindInfo&);
void register_variant(TaskKindId, Affinity, KernelFn);
}
```

### scheduler 的 Task 生命周期

```cpp
namespace me::sched {

struct EvalInstance {
    std::vector<State>                          node_states;
    std::vector<std::vector<InputValue>>        resolved_inputs;
    std::vector<std::vector<OutputSlot>>        output_slots;
    std::atomic<bool>                           cancel_flag;
    Promise<OutputValue>                        terminal_promise;
};

struct Task {                         // scheduler 内部，短命
    NodeRef                     node_ref;
    KernelFn                    kernel;
    const Properties&           props;
    std::span<const InputValue> inputs;
    std::span<OutputSlot>       outputs;
    EvalInstance*               eval;
};

class Scheduler {
public:
    template<typename T>
    Future<T> evaluate_port(const Graph&, PortRef terminal, const EvalContext&);

    void dispatch(Task t) {
        TaskContext ctx;
        ctx.time    = t.eval->time;
        ctx.frames  = &frame_pool_;
        ctx.codecs  = &codec_pool_;
        ctx.gpu     = gpu_for(t);
        ctx.cancel  = t.eval->cancel_flag;
        ctx.cache   = &cache_;
        auto status = t.kernel(ctx, t.props, t.inputs, t.outputs);
        t.eval->resolve(t.node_ref, status);
    }
};
}
```

**关键**：kernel 是纯函数指针无捕获；外部世界通过 `TaskContext` 在 dispatch 时注入；同一 Node 在一次 batch 中产出 N 个 Task（一帧一个 EvalInstance），独立并发。

### 缓存集成

scheduler 在派发每个 Task **前**查缓存：
```cpp
uint64_t key = node.time_invariant
             ? node.content_hash
             : hash_combine(node.content_hash, eval_ctx.time);
if (auto cached = cache.get(key, output_port_idx)) {
    eval->resolve(node_ref, port, *cached);      // kernel 不跑
    return;
}
```

按 `(content_hash, port_idx)` 寻址，多输出节点的每个 output 独立缓存。不同 Graph 结构性等价的节点天然共享缓存条目。

### 取消

`Future::cancel()` → 置 EvalInstance cancel_flag；**不**影响同一 Graph 其他 EvalInstance；kernel 在循环里 poll `ctx.cancel` 自退；`await()` 收到 `ME_E_CANCELLED`。

---

## 批编码：Timeline 按段驱动

**决策：decode/demux 是 Node（在 Graph 里），encode/mux 是 Exporter 自持状态（在 Graph 外）**。

```cpp
Future<void> Exporter::export_to(const OutputSpec& spec, ProgressCb cb) {
    return scheduler_.async_task([this, spec, cb]() {
        auto segments = timeline::segment(*tl_);
        for (const auto& seg : segments) {
            auto g = graph_for_segment(seg);              // cache 命中零开销

            auto video_terminal = g->terminal("video").value();
            auto audio_terminal = g->terminal("audio");   // optional

            for (auto t : frame_times_in(seg, spec)) {
                while (pending_.size() > lookahead_) drain_one();

                auto video_f = scheduler_.evaluate_port(*g, video_terminal, {.time=t});
                std::optional<Future<AudioBuf>> audio_f;
                if (audio_terminal) {
                    audio_f = scheduler_.evaluate_port(*g, *audio_terminal, {.time=t});
                }
                pending_.push_back({t, std::move(video_f), std::move(audio_f)});
            }
        }
        drain_all();
        encoder_.flush();
        muxer_.trailer();
    });
}
```

要点：
- **段边界**触发 Graph 切换——过渡期段和非过渡期段 decode 线程数 / active clip 数都变
- **段内**同一 Graph 被 evaluate 在一串不同 time 上，每次 EvalInstance 独立
- **lookahead pipeline** 是段内的，跨段自然 flush
- **encoder / mux** 顺序 drain，确定性

Previewer / CompositionThumbnailer 类似——先 `find_segment(t)` → 取（或 compile）对应 Graph → `evaluate_port(g, video_terminal, {.time=t})`。

---

## 五模块职责矩阵

| 模块 | 知道 | 不知道 |
|---|---|---|
| `timeline/` | Timeline JSON schema、Segment 切分 | Graph 是什么、Task 怎么跑 |
| `graph/` | Node / Port / Graph / Arena 纯数据 | Kernel 是什么、何时执行 |
| `task/` | TaskKindId、KernelFn 签名、param/port schema | 节点怎么连、何时被调度 |
| `scheduler/` | Task 生命周期、异构池、EvalInstance、缓存查询 | Graph 语义 / 像素 / 字节 |
| `resource/` | FramePool / CodecPool / GpuCtx / Budget | Timeline、Node、Task |
| `orchestrator/` | Timeline → Segments → per-segment Graph；帧编排；encoder/muxer 状态 | 单个 Node 做什么、Task 怎么派 |

违反这个矩阵就是架构腐蚀。新加代码前先对着这张表 check。

---

## 与现有代码的集成

| 现有位置 | 改动 | 对应 backlog |
|---|---|---|
| `src/io/ffmpeg_remux.cpp` | 拆：demux 逻辑注册为 `io::demux` kernel；mux 逻辑归 `orchestrator::MuxerState`（Exporter 持）；passthrough 走 Exporter 的 stream-copy specialization | `refactor-passthrough-into-graph-exporter` |
| `src/api/render.cpp` | `me_render_start` → `Exporter(timeline).export_to(...)`；`me_render_frame` → `Previewer(timeline).frame_at(t)` | 同上 |
| `src/orchestrator/previewer.cpp` | `frame_at(t)` 走 `compile_frame_graph` + `scheduler.evaluate_port` —— graph 框架的第一个 per-frame 真客户。`disk_cache` peek/put 保留作为持久层；in-process 重复访问由 `scheduler.cache` 接管 | shipped |
| `src/io/decode_video_kernel.cpp` | `IoDecodeVideo` kernel：从 `DemuxContext` decode 单帧到 `AVFrame` | shipped |
| `src/compose/convert_rgba8_kernel.cpp` | `RenderConvertRgba8` kernel：`AVFrame` → tightly-packed RGBA8 | shipped |
| `src/scheduler/output_cache.cpp` | `OutputCache` 实现 `(content_hash, time, port)` peek-before-dispatch；`KindInfo::cacheable` 控制；`IoDemux` 不缓存（输出含 mutable AVFormatContext 状态） | shipped |
| `src/timeline/` | 新增 `segmentation.{hpp,cpp}` | `timeline-segmentation` |
| `include/media_engine/*.h` | **不动**——C API 稳定；Graph / Task / Future / Orchestrator 全部内部 | — |
| `src/core/engine_impl.hpp` | 加字段：`FramePool* / CodecPool* / Scheduler*`；engine 管生命周期 | `engine-owns-resources` |

**M1 passthrough 迁移的最小样子**：单段 Timeline，Graph 里一个 `io::demux` Node 作为 terminal（video + audio 两个 packet stream output）；Exporter 的 passthrough specialization 把 packet stream 直接喂给 MuxerState stream-copy。行为 bit-identical。

---

## 明确不做（phase-1 架构定型范围外）

- **Tile-based 执行** —— 全帧起步
- **marl fiber I/O 池** —— 阻塞线程池够用到 M5
- **分布式渲染** —— VISION §4 明确不做
- **动态任务生成（runtime spawn subtask）** —— 所有 Node 在 `compile_segment` 阶段已知
- **暴露 Task / EvalInstance 到公共 API** —— 永远内部
- **Graph-is-a-Node Composite** —— 子图复用只走 builder helpers
- **Editor 作为引擎内建** —— talevia 宿主的责任
- **Streaming Future** —— 单帧 Future + Orchestrator 层做多帧编排
- **跨 Orchestrator Graph 缓存共享** —— 先 per-orchestrator 段缓存，命中率证明必要再提升

---

## 读序建议

1. `docs/VISION.md` §3（工程赌注）—— 理解为什么是这些约束
2. 本文件 §关键理念 + §五模块职责矩阵 —— 职责边界
3. 本文件 §用户面 API —— 对外形态
4. 本文件 §Graph 内部 + §Task 运行时 —— 实现层次
5. 各模块的 `src/<module>/README.md` —— 模块具体当前状态
