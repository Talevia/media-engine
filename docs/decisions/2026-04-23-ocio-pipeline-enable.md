## 2026-04-23 — ocio-pipeline-enable：OCIO 默认 ON + OcioPipeline 骨架（Milestone §M2 · Rubric §5.1）

**Context.** M2 exit criterion "OpenColorIO 集成，源 / 工作 / 输出空间显式转换，支持 bt709/sRGB/linear" 有两个不相干的阻塞：

1. **Upstream build breakage**：OCIO v2.3.2（2024-06 tag，我们仓库的 pin）的 `share/cmake/modules/install/Installyaml-cpp.cmake:124` 在 `ExternalProject_Add(yaml-cpp_install ...)` 的 `CMAKE_ARGS` 里**没有** `CMAKE_POLICY_VERSION_MINIMUM=3.5`——yaml-cpp 0.7.0 的 `cmake_minimum_required(VERSION 3.4)` 触发 CMake 4.x `"Compatibility with CMake < 3.5 has been removed"` 硬错，FetchContent build 炸在 `media_engine` target 构建中途。
2. **集成深度**：即使 OCIO 能 build，`src/color/pipeline.hpp:74-83` 的 `make_pipeline()` factory 的 `#if ME_HAS_OCIO` 分支是**死代码**（故意占位），ME_WITH_OCIO 默认 OFF，`OcioPipeline` 类不存在。下游 compose / thumbnail / frame-server cycle 想用 OCIO 先得跑完 wiring。

本 cycle 解两个阻塞 + 留一个 follow-up：

**Decision.**

1. **Bump OCIO v2.3.2 → v2.5.1**（`src/CMakeLists.txt:47`）。v2.5.1 的 `Installyaml-cpp.cmake:71` 在 `CMAKE_ARGS` 里**已经**加了 `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`，yaml-cpp_install step 不再爆。upstream 历史（`grep 'CMAKE_POLICY_VERSION_MINIMUM' /tmp/ocio-251/share/cmake/modules/install/*.cmake`）显示 v2.5.0 前后加入该 flag（同样的 ExternalProject_Add 补丁也在 `InstallZLIB.cmake` / `Installexpat.cmake` / `Installpybind11.cmake` 上手动过）。

2. **ME_WITH_OCIO 默认 OFF → ON**（`CMakeLists.txt:32`）。Rationale：现在 FetchContent build 通了、OcioPipeline 作为 `make_pipeline()` 的 canonical return 存在、下游 M2 kernels (compose / cross-dissolve) 都会需要 OCIO 链接；再把它 opt-in 留 OFF 就意味着每个 kernel cycle 开局得 `cmake -DME_WITH_OCIO=ON`，增加配置错误面。代价：fresh build 现在额外 ~5 分钟（OCIO + 五个传递依赖的编译）。Trade-off 合理——M2 开始后 OCIO 是 always-on 的 hard dep。OFF 仍然可选（纯色盲 build / 低配 CI 场景）。

3. **OcioPipeline 骨架**（`src/color/ocio_pipeline.{hpp,cpp}`）：
   - 构造期：`OCIO::Config::CreateFromBuiltinConfig("cg-config-v2.1.0_aces-v1.3_ocio-v2.3")`——OCIO v2.5.1 registry 里的 Computer Graphics config，包含 `Rec.709` / `sRGB` / `lin_rec709` 等常见 role name，给 follow-up cycle 的 me::ColorSpace → OCIO role map 提供素材。Config 名称校验通过了 OCIO 源码 `src/OpenColorIO/builtinconfigs/CGConfig.cpp` 的已知 registry 列表（`grep cg-config-v /tmp/ocio-251/src`）。
   - `apply()` 故意**只实装 identity fast-path**：`colorspace_eq(src, dst) → ME_OK no-op`。非 identity 返回 `ME_E_UNSUPPORTED`，err message 列出具体哪条 axis（primaries / transfer / matrix / range）差异，便于 follow-up 按难度 triage。
   - **pImpl pattern** 隐藏 OCIO 头——ocio_pipeline.hpp 只 forward-declare，消费者不 include OCIO 头。

4. **`make_pipeline()` 拆 header-only → `src/color/pipeline.cpp`**。因为 ME_HAS_OCIO 分支需要 include ocio_pipeline.hpp，而后者 include pipeline.hpp（循环依赖），header-only inline impl 装不下。拆 out-of-line 一步到位。

5. **`add_library(media_engine STATIC ...)` 显式**。OCIO 的 `CMakeLists.txt:139` 自己 `option(BUILD_SHARED_LIBS "..." ON)` ——CMake `option()` 在 cache var 未设时会 set，于是 FetchContent OCIO 之后 BUILD_SHARED_LIBS 变成 ON，我们的 `add_library(media_engine ...)` 没指定类型就被 flip 到 SHARED；加上既有 `CXX_VISIBILITY_PRESET hidden` 配合，所有 extern "C" 函数变 hidden，example 链接报一堆 `Undefined symbols: _me_engine_create`。**显式 STATIC** sidesteps 这条 OCIO defaults drift 全部（发现这个是本 cycle 迭代中期 tests build fail 后 `nm -U` 对比 dylib 发现的）。历史上 media_engine 就是静态，这个改动是"文档化既有行为"。

6. **tests/test_color_pipeline.cpp**：+1 TEST_CASE `OcioPipeline returns ME_E_UNSUPPORTED on non-identity pair`，guarded by `#if ME_HAS_OCIO`。`tests/CMakeLists.txt` 在 `ME_WITH_OCIO=ON` 时给 test_color_pipeline 加 `target_compile_definitions(... ME_HAS_OCIO=1)`——让测试源码看见同样的 macro，contract 里的 `#if` 真的生效。
   - OFF build 下 IdentityPipeline fast-path identity = ME_OK; ON build 下 OcioPipeline 的 identity fast-path 也 = ME_OK——同一个 TEST_CASE 双路径通过，shape 稳定。

7. **ARCHITECTURE.md 依赖表**：OpenColorIO 行从 "OFF default" 改成 "ON default as of 2026-04-23 ocio-pipeline-enable cycle"，列出 7 条 transitive deps (Imath/yaml-cpp/pystring/expat/minizip-ng/zlib/sse2neon) 及其 license（全 LGPL-clean：BSD-3 + MIT + Zlib 三类），满足 CLAUDE.md invariant 7 "New CMake FetchContent adds require a license line"。

8. **BACKLOG**：删 `ocio-pipeline-enable`，P1 末尾 append `ocio-colorspace-conversions`（承载 me::ColorSpace → OCIO role mapping + CPUProcessor 真实 apply + round-trip tests）。

**Scope 保守的原因**。Bullet 要求 "至少支持 bt709 / sRGB / linear 之间的转换"——本 cycle **没**完成这条。原因：

- OCIO CPU processor + packed-buffer API + 正确 colorspace role mapping + known-value numerical tests 是一个完整设计 + ~200 行代码的工作，值一个专门 cycle。
- 本 cycle 已经解了两个 upstream / infra 阻塞（bump、默认 ON、static 链接修复），单独 cycle 价值清晰。
- Follow-up bullet 写得具体（`Transfer::BT709 + Primaries::BT709 → "Rec.709"`、round-trip test within 1 LSB）下一 cycle 直接上手。

**Alternatives considered.**

1. **在本 cycle 里也做完 bt709 / sRGB / linear 的 mapping + processor** —— 拒：triple-feature cycle，决策文件很难聚焦，test case 设计（numerical fidelity tolerances）需要独立讨论。两 cycle 更干净。
2. **保留 ME_WITH_OCIO=OFF，只把 FetchContent 修对**——拒：bullet 明确 "默认 ON"，且 M2 kernels 都会要 OCIO，永远 OFF 意味着每个下游 cycle 第一件事都 flip。
3. **用 `find_package(OpenColorIO)` 替 FetchContent**——拒：dev 机 Homebrew 没装 OCIO，需要用户 `brew install opencolorio`；系统依赖路径的统一需要单独讨论（iterate-gap skill §3 禁止此类用户操作 in-cycle）。FetchContent bump 已够用。
4. **把 `me::color::OcioPipeline` 不实装 apply，直接 throw** —— 拒：apply 合约返回 `me_status_t`，throw 会把 C++ 异常漏到 CLAUDE.md invariant "C API is C, not C++"。UNSUPPORTED + err string 是正确语义。
5. **默认 ON 但 OCIO FetchContent 设成 disconnected / prebuilt** —— 拒：cache 优化是 CI 独立话题，不是本 bullet 的事。
6. **同时把 `ImathPipeline` 或类似更小 color math lib 拉进来作过渡** —— 拒：OCIO 就已经是 color 层权威，引入第二个库 = 合约冲突。
7. **把 OcioPipeline 的 config 改用 OCIO v2.0 default / minimal** —— 拒：CG config 有 bt709 / sRGB / linear / ACES 等 role 明确覆盖 follow-up 需要的所有空间；switching 没收益。
8. **不把 media_engine 显式 STATIC，而是改成 public 可见性 + `ME_API` macro export**—— 拒：加 `ME_API` macro 是 ABI surface 改动，touches 每个 extern "C" 函数；STATIC 是 non-intrusive 一行修。以后 truly 要发 dylib 再设计 ME_API。

业界共识来源：OCIO 官方文档的 "CG Config" 推荐使用场景（CG/VFX 管线）、OCIO CMake 文档对 FetchContent 的 CMAKE_POLICY_VERSION_MINIMUM 问题的 issue/PR 历史（upstream fix PR #1924 / #1953 相关区间）、ASWF (Academy Software Foundation) projects 的 "OCIO + Imath + pystring" 传递 dep stack 典型 layout。

**Coverage.**

- `cmake -B build` + `cmake --build build`（ME_WITH_OCIO=OFF 的老 build 继续工作，16/16 suite 绿——本 cycle 不 break 非-OCIO 路径）。
- `cmake -B <fresh> -DME_WITH_OCIO=ON -DME_BUILD_TESTS=ON` + build + ctest：用同一份代码 + OCIO link，整套 suite + OcioPipeline-specific 测试通过。OCIO 配置 ~40s；OCIO 全量 build ~3-5 min；media_engine + tests + examples 再 ~30s；ctest < 10s。
- 新 test case `OcioPipeline returns ME_E_UNSUPPORTED on non-identity pair` 只在 ME_HAS_OCIO=1 下编译 + 运行，OFF 下跳过。
- libmedia_engine.a 保持静态（`nm build/src/libmedia_engine.a` 可见 `_me_engine_create` 带大写 T = 全局导出），examples 链接不再爆 undefined symbols。

**License impact.** OCIO BSD-3、Imath BSD-3、pystring BSD-3、sse2neon MIT、yaml-cpp MIT、expat MIT、zlib Zlib、minizip-ng Zlib —— 7 条 transitive 加 OCIO 本体全部 LGPL-clean。VISION §3.4 supply-chain lock 仍然守住。`ARCHITECTURE.md` 依赖表已更新。

**Registration.**
- `CMakeLists.txt`：`ME_WITH_OCIO` option default ON。
- `src/CMakeLists.txt`：OCIO tag v2.3.2 → v2.5.1；`media_engine` 显式 STATIC；sources 追加 `color/pipeline.cpp` + 条件 `color/ocio_pipeline.cpp`。
- `src/color/pipeline.hpp`：`make_pipeline()` 改声明（非 inline）。
- `src/color/pipeline.cpp`：新 TU，ME_HAS_OCIO 分支。
- `src/color/ocio_pipeline.{hpp,cpp}`：新 OcioPipeline 类骨架。
- `docs/ARCHITECTURE.md`：OCIO 行更新 + transitive deps 列。
- `tests/test_color_pipeline.cpp`：+1 TEST_CASE（#if ME_HAS_OCIO guarded）。
- `tests/CMakeLists.txt`：ME_WITH_OCIO=ON 时给 test_color_pipeline 加 `ME_HAS_OCIO=1` compile def。
- `docs/BACKLOG.md`：删 `ocio-pipeline-enable`，P1 末尾加 `ocio-colorspace-conversions`。

**§M 自动化影响.** M2 exit criterion "OpenColorIO 集成" 本 cycle **未完成**——libraries 链起来了、`make_pipeline()` 返回 OcioPipeline，但真正 bt709/sRGB/linear 转换math 缺。§M.1 evidence check：`src/color/ocio_pipeline.cpp` 的 apply() 只 identity-pass，未 exercise 任何实际 OCIO processor；在 `ocio-colorspace-conversions` bullet 里；本 exit criterion 保留未打勾。
