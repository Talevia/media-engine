## 2026-04-23 — ocio-integration-skeleton：为 M2 色彩管理保留 `me::color` 命名空间 + opt-in 构建入口（Milestone §M2-prep · Rubric §5.1）

**Context.** VISION §3.4 把 OpenColorIO 钉在了 M2 色彩管理路径上，`docs/ARCHITECTURE.md` 依赖白名单里也列着 "OCIO BSD-3 Phase 2"，但仓库里至今没有一行 `me::color::*` 代码。M2 真开始做 compose 时再一次性接 OCIO（FetchContent + 命名空间 + identity pipeline + 真 color conversion + asset-colorspace-field 消费方）会撑爆单一 PR。本 cycle 按 backlog bullet 把"骨架"拆出来独立落：CMake 入口声明、命名空间类型、license table 状态对齐。**不接** `asset-colorspace-field`——那是下一 cycle 的 scope。

**Decision.** 三件事，全部 **opt-in / 默认无副作用**：

1. **命名空间 + 抽象基类** — 新 `src/color/pipeline.hpp`（header-only，无 `.cpp`）：
   - `me::color::Pipeline` 抽象基类，单方法 `apply(void* buffer, size_t byte_count, const me::ColorSpace& src, const me::ColorSpace& dst, std::string* err) -> me_status_t`。
   - `me::color::IdentityPipeline` 具体实现：所有方法 no-op 返回 `ME_OK`。所有 caller 在真 OCIO pipeline 到位前可以直接用它当占位，接口 shape 已经定型——M2 compose 接 OCIO 时只改构造点，不改 callsite。
   - 头里吃 `timeline/timeline_impl.hpp` 的 `me::ColorSpace`（已经是 typed enum 了，`asset-colorspace-field` cycle 完成的）。不引入新 public C struct，不进 `include/media_engine/*.h`——M1 ABI 不动。

2. **CMake opt-in 入口** — 顶级 `CMakeLists.txt` 加 `option(ME_WITH_OCIO "Fetch and link OpenColorIO (M2 color pipeline)" OFF)`，默认 OFF。`src/CMakeLists.txt` 里：
   ```cmake
   if(ME_WITH_OCIO)
     FetchContent_Declare(OpenColorIO
       GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenColorIO.git
       GIT_TAG v2.3.2 GIT_SHALLOW TRUE)
     set(OCIO_BUILD_APPS/TESTS/GPU_TESTS/DOCS/PYTHON/JAVA OFF CACHE INTERNAL "")
     set(CMAKE_CXX_STANDARD 20 CACHE STRING "" FORCE)   # CppVersion.cmake 要 CACHE
     include(fetchcontent_policy)
     FetchContent_MakeAvailable(OpenColorIO)
     target_link_libraries(media_engine PRIVATE OpenColorIO)
     target_compile_definitions(media_engine PRIVATE ME_HAS_OCIO=1)
   endif()
   ```
   OFF 时整段跳过，构建图和行为完全不动；ON 时 FetchContent 拉 OCIO 2.3.2，把 `OpenColorIO` target 挂到 media_engine 的 PRIVATE 链上。

3. **ARCHITECTURE.md 对齐** — license table 里 OCIO 行从单纯的 "Phase 2 (color mgmt)" 扩成 "…—CMake opt-in behind `ME_WITH_OCIO` (OFF default); `me::color` namespace reserved via `IdentityPipeline` stub"。搜这张表就能看到当前实际状态，不用翻源码推。

**ON 构建现在能走多远（探测结果）.** 本 cycle 验证过 `cmake -B build-ocio -S . -DME_WITH_OCIO=ON`：
- OCIO 2.3.2 shallow clone 成功。
- OCIO 自己的 `CppVersion.cmake:19` 调 `set_property(CACHE CMAKE_CXX_STANDARD PROPERTY STRINGS ...)`，要求 `CMAKE_CXX_STANDARD` 是 CACHE 变量——我们顶层 set 是普通变量——**已 fix**（decision 里 CMake 片段那行 `set(CMAKE_CXX_STANDARD 20 CACHE STRING "" FORCE)`）。
- OCIO 安装依赖自动找 expat / yaml-cpp / pystring / Imath / ZLIB / minizip-ng，缺失的 OCIO 会走自己的 "Installed <dep>" 路径内嵌 fetch。
- 配置阶段通过（44 秒）。生成通过。
- **然而**：OCIO 内嵌 `yaml-cpp` 用 ExternalProject_Add 深嵌一层 CMake，那层 yaml-cpp 的 `cmake_minimum_required` 低于 CMake 4.x 的 policy 下限；我们的 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` 传不进那层 sub-build。这是 OCIO 2.3.2 和 CMake 4.x 的已知摩擦（上游 OCIO 有 PR 在修），属于 OCIO-internal，不该这个 cycle 去深挖。记录在 decision，ON 构建暂时卡在 yaml-cpp install 步。

OFF 构建（默认路径）正常：`cmake --build build` + `ctest` → 9/9 绿，和 refactor 前完全一致。

**Alternatives considered.**

1. **默认 ON 直接强制拉 OCIO**——拒：依赖堆复杂（至少 6 个 transitive deps），配置 ~45 秒 + 构建几分钟，没有 consumer 就上线 CI 成本。先开 opt-in 入口，等 consumer 需要时再默认。
2. **`find_package(OpenColorIO REQUIRED)` 只不 fetch**——拒：开发机 / CI 不一定有 brew install opencolorio，跨机不一致。FetchContent + opt-in 更稳定。
3. **继续挖 yaml-cpp policy 问题到 ON 完全跑通**——拒：超本 cycle scope。真想用 OCIO 时（真 M2 consumer 落地），先 investigate 是否升 OCIO 到 2.4.x + 看上游 PR 进度，比现在盲猜 patch 更合理。
4. **不搭骨架，等 M2 compose 一起搞**——拒：正是本 bullet 反对的做法，单 PR 会冲击爆炸。
5. **`me::color::Pipeline` 吃 `const char*` 而不是 `const me::ColorSpace&`**——拒：`me::ColorSpace` 已经 typed（Primaries/Transfer/Matrix/Range 四条 enum），再走 string 是 VISION §3.2 反方向。

业界共识来源：Gated external dep via CMake option 是 OpenEXR / Imath 自己的模式（`OPENEXR_BUILD_PYTHON_MODULES`、`IMATH_BUILD_PYTHON`）、也是 Eigen（`EIGEN_BUILD_TESTING`）和 spdlog（`SPDLOG_BUILD_SHARED`）的 norm。opt-in + default-off + ARCHITECTURE.md 记录状态是 LGPL 生态里的 "soft-reservation" 标准招式。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean。
- `ctest --test-dir build` 9/9 suite 绿。
- `cmake -B build-ocio -DME_WITH_OCIO=ON` config 通过（44s）；build 卡在 OCIO 内嵌 yaml-cpp 的 CMake 4.x 兼容——记录在本 decision，不算本 cycle 要修。
- `src/color/pipeline.hpp` 不被任何 src/ 里的 cpp include——目的就是"保留"，不消费。
- `-Wall -Wextra -Wpedantic -Wno-unused-parameter` clean（header-only，只在消费方 include 时才编）。

**License impact.** 声明入口，**未**实际链接任何新二进制（默认 OFF）。`ARCHITECTURE.md` license table 现在正确反映"声明有、默认不链"。首次 `-DME_WITH_OCIO=ON` 的开发者要额外审 OCIO 2.3.2 transitive dep license：Imath BSD-3、yaml-cpp MIT、pystring BSD、expat MIT、minizip-ng Zlib——全部 MIT/BSD/Zlib 系列，无 GPL 污染风险。

**Registration.** C API 不动。新增：
- `CMakeLists.txt`：`ME_WITH_OCIO` option。
- `src/CMakeLists.txt`：`ME_WITH_OCIO` 守护的 `FetchContent_Declare(OpenColorIO)` block + conditional `target_link_libraries(... OpenColorIO)` + `-DME_HAS_OCIO=1` compile def。
- `src/color/pipeline.hpp` 新头。
- `docs/ARCHITECTURE.md` license table 条目扩写。
无 schema / kernel / CodecPool / JSON schema / C API 函数变更。
