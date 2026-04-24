## 2026-04-23 — bgfx-integration-plan：future bgfx FetchContent cycle 的执行计划（Milestone §M3 · Rubric §5.3）

**Context.** `bgfx-integration-skeleton` bullet 在过去 session 被 pivot 6 次。理由一致：bgfx FetchContent + bgfx::Init + Metal 上下文 + 初次编译是 half-session+ 级工程（bgfx 自带 non-CMake 构建 + ~50MB 源码 + transitive bx / bimg 依赖 + macOS Metal SDK + Windows DX11 fallback 等跨平台），单 cycle 塞不下。本 cycle **不写代码**——只落下一份执行计划，让未来专门的 bgfx cycle（可能需要 2-4 小时专注时间）拿着清单直接动手，而不需要重新 research。

Pre-cycle state：
- `gpu-backend-skeleton` (0313255) 已落：`me::gpu::GpuBackend` 抽象 + `NullGpuBackend` + `make_gpu_backend()` factory + CMake `ME_WITH_GPU` option（默认 OFF，占位）。
- ARCHITECTURE.md 的依赖白名单表已预留 "bgfx BSD-2 Phase 3 (GPU backend)" 行（`docs/ARCHITECTURE.md:84`）。
- BACKLOG bullet 列了 5 条剩余工作但未分解到可执行步骤。

**Decision.** 本文件 = bgfx 集成的 executable plan。未来 cycle 按步骤执行。

---

### 1. CMake 集成策略：bkaradzic/bgfx.cmake wrapper

bgfx 自家构建（GENie + makefile + script）不是 CMake-native，无法直接 FetchContent。业界标配是 **bkaradzic/bgfx.cmake** —— 由 bgfx 作者本人维护的 CMake wrapper，fetches bgfx + bx + bimg，提供 `bgfx` / `bx` / `bimg` CMake targets 可直接 `target_link_libraries`。替代方案评估：

- ✅ **bkaradzic/bgfx.cmake** (选定)：官方维护，API 稳定，社区使用广泛（imgui、raylib 等）。FetchContent_Declare + MakeAvailable 直接用。
- ❌ **Vendor bgfx 源码到 third_party/**：破坏 "新 dep 不 commit 源码" 约定（VISION §3.4 + CLAUDE.md 隐含）；upgrade 麻烦；license 文件管理。
- ❌ **find_package(bgfx)**：bgfx 没有 CMake 标准 find_package 模块，用户得自己 install bgfx（非可选依赖体验差）。

**固定 version**: `bkaradzic/bgfx.cmake` 的 commit hash，不用 `master`（CLAUDE.md 硬规：每个 dep 必须 commit-pin）。选定时用 `bgfx.cmake` 最近 tag 或近期 green commit。建议：**tag `1.128.8786-475`**（最近稳定 release，兼容当前 bx/bimg）；具体 pin 在执行 cycle 时 `git ls-remote` 最新 tag 再定。

### 2. CMake 代码骨架（执行 cycle 时落 `CMakeLists.txt`）

```cmake
# 根 CMakeLists.txt 现有 ME_WITH_GPU option 后插入：
if(ME_WITH_GPU)
    FetchContent_Declare(bgfx_cmake
        GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
        GIT_TAG        v1.128.8786-475   # PIN — update in its own commit only
    )
    FetchContent_MakeAvailable(bgfx_cmake)
    # After this, `bgfx`, `bx`, `bimg` targets are available.
    # media_engine target will `target_link_libraries(... bgfx bx bimg)` in src/CMakeLists.txt.
endif()
```

**Risk**：FetchContent_MakeAvailable(bgfx_cmake) 会 clone ~50 MB（bgfx + bx + bimg 源码）+ 编译（bx ~3min、bimg ~5min、bgfx ~10min on M1 dev hardware）。首次 configure 5-10 分钟。Cache 后 rebuild 秒级，但冷构建成本显著。缓解：

- 默认 `ME_WITH_GPU=OFF`：developer opt-in，CI 可走 OFF 路径跳 bgfx。
- 执行 cycle 里 benchmark 首次 configure 时间，写进 decision doc。
- 考虑加 CMake cache 优化：设 `BGFX_BUILD_EXAMPLES=OFF` / `BGFX_BUILD_TOOLS=OFF` 让 wrapper 少编 demos / `texturec`/`geometryc` 工具。

### 3. `BgfxGpuBackend` 实装骨架（执行 cycle 时落 `src/gpu/bgfx_gpu_backend.{hpp,cpp}`）

```cpp
// bgfx_gpu_backend.hpp
#include "gpu/gpu_backend.hpp"
#if ME_HAS_GPU
#include <bgfx/bgfx.h>
#endif

namespace me::gpu {

class BgfxGpuBackend final : public GpuBackend {
public:
    BgfxGpuBackend() noexcept;  // calls bgfx::init + bgfx::setViewClear
    ~BgfxGpuBackend() override;  // calls bgfx::shutdown if init succeeded

    bool        available() const noexcept override { return ok_; }
    const char* name()      const noexcept override { return name_; }

private:
    bool        ok_   = false;
    const char* name_ = "bgfx-uninit";
};

}
```

Constructor 职责：
- `bgfx::Init init;` — configure: `init.type = bgfx::RendererType::Metal` (macOS) / `Vulkan` (Linux) / `Direct3D11` (Windows) / `RendererType::Count` (auto). Phase-1 试 Metal first, fallback to auto if init returns false.
- `init.resolution.width = 1; init.resolution.height = 1;` — 我们是 headless offscreen renderer; 最小 backbuffer。
- `init.platformData` — **skip**。bgfx 支持 headless init 当没 window handle（某些 renderer 还是要 offscreen context）；Metal renderer 会 auto-create `MTLDevice`。
- `if (!bgfx::init(init)) { ok_ = false; return; }`。
- `bgfx::setViewClear(0, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);`。
- `ok_ = true; name_ = "bgfx-metal";`（根据 `bgfx::getCaps()->rendererType` 填 name）。

Destructor: `if (ok_) bgfx::shutdown();`。

### 4. Factory 分支 (`src/gpu/gpu_backend.cpp`)

```cpp
std::unique_ptr<GpuBackend> make_gpu_backend() {
#if ME_HAS_GPU
    auto backend = std::make_unique<BgfxGpuBackend>();
    if (backend->available()) return backend;
    // bgfx init failed (likely Metal SDK missing / HW too old); fall back to null.
#endif
    return std::make_unique<NullGpuBackend>();
}
```

`ME_HAS_GPU` 由 CMake 传入（需要在 `src/CMakeLists.txt` 加 `target_compile_definitions(media_engine PRIVATE $<$<BOOL:${ME_WITH_GPU}>:ME_HAS_GPU=1>)`）。

### 5. Engine 挂载 (`src/core/engine_impl.hpp`)

```cpp
struct me_engine {
    // existing members ...
    std::unique_ptr<me::gpu::GpuBackend> gpu;
};

// src/api/engine.cpp me_engine_create:
eng->gpu = me::gpu::make_gpu_backend();
```

### 6. ARCHITECTURE.md 更新

- Dependency table bgfx 行更新：`Phase 3 (GPU backend)` → `M3 (opt-in behind ME_WITH_GPU). Version: <pinned>. License BSD-2.`。
- Add section "GPU Backend": describes ME_WITH_GPU flag, init semantics, Metal/Vulkan/DX11 renderer selection, the fact that media_engine is headless (no window; bgfx created in offscreen mode).

### 7. Tests

- `tests/test_gpu_backend.cpp` 扩展：`#if ME_HAS_GPU` 下的 case 验证 `name()` 不是 "null"（e.g. 包含 "bgfx"）。OFF 下继续 null-only。
- 不加 CI-dependent tests（bgfx init 需要 GPU，在 headless CI 可能失败）；本地 dev machine 通过即可。

### 8. Validation checklist（执行 cycle 结尾核对）

- [ ] `cmake -B build -DME_WITH_GPU=OFF -S .` + `cmake --build build` —— 行为完全等价 pre-cycle（backward compat）。
- [ ] `cmake -B build-gpu -DME_WITH_GPU=ON -S .` + 首次 configure 成功 + 编译成功（时间 <15 min on M1）。
- [ ] `ctest --test-dir build-gpu -R test_gpu_backend` —— `available()` 返 true, `name()` 含 "bgfx" 或 "metal"。
- [ ] `me_engine_create` 不 regress（test_engine 绿）。
- [ ] 清 build-gpu/ + re-configure 仍工作（rebuild stable）。

---

### 关键 non-goals（执行 cycle 内不做）

1. 任何 effect render（blur / color-correct / LUT）—— 自己的 bullet（`effect-gpu-*`）。
2. EffectChain GPU merger / pass 合并 —— 自己的 bullet。
3. GPU texture upload pipeline（YUV → GPU texture）—— 自己的 bullet（GPU compose 接入 ComposeSink）。
4. Linux / Windows backend polish —— M3 只要求 macOS Metal 可渲染。

### 建议执行 cycle 的实际开销

基于上面 plan：
- CMake + FetchContent 接线：30 分钟（包含首次配置 5-10 min compile）。
- BgfxGpuBackend class + constructor/destructor：20 分钟。
- Factory 分支 + engine 集成：15 分钟。
- ARCHITECTURE.md + decision doc：20 分钟。
- 测试 + debug + validation：30-60 分钟（取决于 Metal init 能否 one-shot 成功 + bgfx 版本兼容性）。

**Total: 2-3 小时 focused work**。建议单独安排一个 session，而不是在 iterate-gap 多任务 cycle 里挤。

**Alternatives considered.**

1. **把 plan 内容直接塞 bullet 文本** —— 拒：bullet 留 succinct 指向 decision doc 更合格式；~150 行的 exec plan 在 bullet 里 dilute 优先级视图。
2. **合 plan + 部分代码（CMake option propagate）** —— 拒：本 cycle 的价值是"plan clarity"；塞代码半做半搁破坏 "plan-only" 的干净。
3. **Wait for user to flag bgfx priority** —— 拒：bullet 已在 P1 顶端 6 cycle；pivot 已暗示 "not happening in regular iterate-gap loop"。显式 plan 反而给用户 actionable info：要么给这个 cycle 留时间、要么降级 bullet 到 P2。
4. **Use find_package(bgfx) instead of FetchContent bkaradzic/bgfx.cmake** —— 拒（见 §1 评估）。

**Scope 边界.** 本 cycle **交付**：
- Exec plan doc（本文件）。

本 cycle **不做**：
- 任何代码改动。
- BACKLOG bullet 关闭或 narrow（bullet 指向 plan，等执行 cycle 完成再 close）。

**Coverage.** 不需（docs-only）。

**License impact.** 无。

**Registration.** 只本文件一个新文件。

**§M 自动化影响.** M3 current milestone，plan doc 不解锁 exit criterion。§M.1 不 tick。

**Action item for next session.** User / maintainer 见此 plan 后可（a）给 bgfx 安排专 session（推荐）；（b）降级 bullet 到 P2 延后；或（c）拒绝 bgfx 方案重新设计（e.g. 直接用 Metal Cocoa framework skip bgfx 抽象——不推荐，会 lock macOS-only）。iterate-gap loop 在 bullet 不关前继续绕过它。
