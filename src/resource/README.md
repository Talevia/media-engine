# src/resource/

**职责**：消除 per-frame malloc；统一 refcount；限制总内存；为 scheduler 提供 backpressure 信号。FramePool / CodecPool / GpuContext / Budget。

See `docs/ARCHITECTURE_GRAPH.md` §Task 运行时 (TaskContext injection) for how resources reach kernels.

## 当前状态

**Scaffolded, impl incoming**——本目录只有这个 README。具体文件由 backlog `engine-owns-resources` 实装。

## 计划的文件

- `frame_pool.hpp` / `frame_pool.cpp` — `FrameHandle`（refcounted `shared_ptr<FrameBuffer>`）+ `FramePool::acquire(FrameSpec)` + pressure 接口
- `codec_pool.hpp` / `codec_pool.cpp` — `AVCodecContext` 复用，避免重复 `avcodec_open2`
- `gpu_context.hpp` / `gpu_context.cpp` — bgfx init / 复用（stub，M3 填）
- `budget.hpp` / `budget.cpp` — 内存预算 + LRU eviction

## 关键数据类型

```cpp
enum class PixelFormat : uint8_t { Rgba8, Rgba16F, Nv12, P010, Yuv420p };
enum class Device      : uint8_t { Cpu, GpuBgfx, HwDecoderSurface };

struct FrameSpec { int width, height; PixelFormat fmt; Device dev; };

class FrameHandle {
    // shared_ptr<FrameBuffer> 包装
    // data() 仅 CPU；gpu_texture() 仅 GPU
    // refcount 归 0 自动回池
};
```

## 边界

- ❌ 不关心 Node / Graph / Task（kernel 拿到 `FrameHandle` 就够了）
- ❌ 不做色彩管理（那是 M2 的 OCIO 层）
- ❌ 不做编解码（那是 kernel 的事；CodecPool 只管 context 复用）
