## 2026-04-23 — gpu-backend-skeleton：scope-A of bgfx-integration-skeleton（GpuBackend 抽象 + NullGpuBackend 默认）（Milestone §M3 · Rubric §5.3）

**Context.** Bullet `bgfx-integration-skeleton` 自 M3 开局挂在 P1 顶端，已连续 5 cycle 被跳过——原因是"bgfx FetchContent + Metal 初始化 + CMake wrapper 选择（bkaradzic/bgfx.cmake vs vendoring）"是半 session 级工程，超过单 cycle 承受。本 cycle 切出**最小可行的第一步**：不拉 bgfx 依赖，先立 `GpuBackend` C++ 抽象 + `NullGpuBackend` 默认 + CMake `ME_WITH_GPU` 占位 option。未来 cycle 添加 bgfx FetchContent + `BgfxGpuBackend` subclass，factory 在 `ME_WITH_GPU=ON` 时返 bgfx-backed 实例。

Before-state grep evidence：
- `grep -rn 'bgfx\|GpuBackend\|ME_WITH_GPU' src include CMakeLists.txt cmake 2>/dev/null` 返回空（除 docs/ARCHITECTURE.md 已有的 "Phase 3" planning 行）。
- 无 `src/gpu/` 目录。
- 无 CMake `ME_WITH_GPU` option。

**Decision.**

1. **`src/gpu/gpu_backend.hpp`** 新文件，`me::gpu::GpuBackend` 抽象类：
   - `virtual ~GpuBackend() = default;`
   - `virtual bool available() const noexcept = 0;` —— 唯一 phase-1 方法；false = CPU fallback only。
   - `virtual const char* name() const noexcept = 0;` —— 调试 / log identifier。
   - `std::unique_ptr<GpuBackend> make_gpu_backend();` —— factory。

2. **`src/gpu/null_gpu_backend.hpp`** 新 header-only，`NullGpuBackend final : GpuBackend`：
   - `available()` → false；`name()` → `"null"`。

3. **`src/gpu/gpu_backend.cpp`** 实装 `make_gpu_backend()` 返 `NullGpuBackend`（phase-1 仅此一种）。

4. **`CMakeLists.txt`** 根目录 + `option(ME_WITH_GPU "Enable bgfx GPU backend (M3, currently stub)" OFF)`。注释明写"flipping ON today changes nothing visible"——未来 cycle 加真实 bgfx FetchContent + Metal 初始化时才有副作用。

5. **`src/CMakeLists.txt`** + `gpu/gpu_backend.cpp` 到 media_engine sources（无条件编译——null backend 永远需要；bgfx backend 未来加 `if(ME_WITH_GPU) ... endif()` 块）。

6. **Tests** (`tests/test_gpu_backend.cpp`) —— 4 TEST_CASE / 204 assertion（大部分来自 smoke loop 的 100 次 call）：
   - `make_gpu_backend` 返非 null。
   - `available()` = false。
   - `name()` = "null"。
   - 100 次反复调 `available()` + `name()` 结果不变（stateless smoke）。

7. **NOT yet integrated** 进 me_engine / ComposeSink。**下一 bgfx cycle** 负责：
   - `FetchContent_Declare(bgfx)` via bkaradzic/bgfx.cmake wrapper（bgfx 自家 makefile 非 CMake，wrapper 是业界实践）。
   - `BgfxGpuBackend : GpuBackend` 实装 init/shutdown + Metal context creation on macOS。
   - `make_gpu_backend()` 分支：`#if ME_WITH_GPU` → `BgfxGpuBackend`，else → `NullGpuBackend`。
   - me_engine 持 `std::unique_ptr<GpuBackend>` 字段；ComposeSink 构造时 check `gpu->available()`，true 走未来 GPU compose 路径（更新另一 bullet 实装），false 走现有 CPU 路径。

**Alternatives considered.**

1. **一次性上完 bgfx FetchContent + init** —— 拒：已连续 5 cycle 被跳过的原因。单 cycle 做 bgfx 的 build system（bgfx 用 bx + bimg + bgfx 三组件 + 自定义 makefile）+ CMake wrapper 选择 + macOS Metal SDK + 实际渲染 clear buffer，实测半 session 以上，session-risk 不 acceptable。
2. **`ME_WITH_GPU=ON` 今 cycle 就拉 bgfx 并编译成功** —— 拒：依然是 bgfx 集成本身的 scope。本 cycle 只立接口 + 占位 option，真 dep 留给专门 cycle。
3. **GpuBackend 加 texture / frame 方法**占位 —— 拒：phase-1 谁用不了 texture 方法，方法签名会在 bgfx 集成时根据实际 API 决定。YAGNI——只加 `available()` 让 caller 分岔。
4. **把 GpuBackend 做成 C API `me_gpu_backend_t`** —— 拒：内部抽象，不跨 ABI。等 host SDK 需要查 GPU 信息时（e.g. `me_engine_gpu_name()`）再开 C API。
5. **FetchContent_Declare(bgfx) 本 cycle 加，但 guarded `if(ME_WITH_GPU)`** —— 拒：即便 guarded，未来某个 reviewer `ME_WITH_GPU=ON` 会触发 50+MB FetchContent、5-10 min 首次配置。真正加 FetchContent 时要同步加 cache check + 降频次配置开销。本 cycle 仅 option + null backend，零依赖成本。
6. **ARCHITECTURE.md 白名单立刻加 bgfx 实际行** —— 已有 "Phase 3 (GPU backend)" 行预留；本 cycle 不改（真 dep 未加）。

**Scope 边界.** 本 cycle **交付**：
- `GpuBackend` / `NullGpuBackend` 抽象 + factory。
- CMake `ME_WITH_GPU` option 占位。
- 4 单元测试。

本 cycle **不做**：
- bgfx FetchContent 或任何 bgfx 代码接触。
- Metal context 初始化。
- me_engine / ComposeSink 的 GpuBackend 字段集成。

Bullet `bgfx-integration-skeleton` **保留**——核心 scope（bgfx init 成功 + clear backbuffer + shutdown）未动；只拆出了"接口形状"。narrow 文本反映 GpuBackend skeleton 已就位，剩余 bgfx FetchContent + BgfxGpuBackend。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 30/30 suite 绿（+1 suite `test_gpu_backend`）。
- `test_gpu_backend` 4 case / 204 assertion（100 次 smoke loop × 2 assertion + 3 basic case × 2 = 204）。

**License impact.** 无（pure C++ + CMake option）。

**Registration.**
- `src/gpu/gpu_backend.hpp`：新文件（抽象）。
- `src/gpu/null_gpu_backend.hpp`：新文件（header-only null impl）。
- `src/gpu/gpu_backend.cpp`：新文件（factory）。
- `CMakeLists.txt`：+ `option(ME_WITH_GPU ...)`。
- `src/CMakeLists.txt`：+ `gpu/gpu_backend.cpp`。
- `tests/test_gpu_backend.cpp`：新文件。
- `tests/CMakeLists.txt`：+ test suite + src include。
- `docs/BACKLOG.md`：bullet `bgfx-integration-skeleton` narrow——GpuBackend 抽象 + Null default + ME_WITH_GPU option 已就位，剩余 bgfx FetchContent + BgfxGpuBackend impl + Metal init。

**§M 自动化影响.** M3 exit criterion "bgfx 集成，macOS Metal 后端可渲染" 本 cycle **未满足**——只立接口形状，未接 bgfx / Metal。§M.1 不 tick。
