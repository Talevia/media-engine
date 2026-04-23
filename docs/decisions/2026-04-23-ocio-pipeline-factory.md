## 2026-04-23 — ocio-pipeline-factory：`me::color::make_pipeline()` 收口（Milestone §M2-prep · Rubric §5.1）

**Context.** `ocio-integration-skeleton` cycle 落了 `me::color::Pipeline` 抽象基类 + `IdentityPipeline` 具体实现，但没有 factory——callers 到了真需要 color pipeline 的那天还得自己 `std::make_unique<IdentityPipeline>()`。问题：等到 `OcioPipeline` 真写出来时，callers 要全部改成 `std::make_unique<OcioPipeline>(...)` 或者 `#if ME_HAS_OCIO`。散落在所有消费方的 branching 是 pain point。再考虑到后续可能加 `MetalColorPipeline` / `SimdColorPipeline` 等 variant，决定 factory 选哪个的逻辑应该**集中一处**。

**Decision.** `src/color/pipeline.hpp` 末尾加一个 inline factory：

```cpp
inline std::unique_ptr<Pipeline> make_pipeline() {
#if ME_HAS_OCIO
    /* Reserved branch — wire me::color::OcioPipeline here when it lands. */
    return std::make_unique<IdentityPipeline>();  /* placeholder */
#else
    return std::make_unique<IdentityPipeline>();
#endif
}
```

`ME_HAS_OCIO` 已经是 `src/CMakeLists.txt` 在 `ME_WITH_OCIO=ON` 时注入的 compile def（在 `ocio-integration-skeleton` 里定义）。今天两个 branch 返回同一个 Identity（因为 OcioPipeline 尚未实现），但**形状**已经到位：下一次 OCIO cycle 只需要把 `#if ME_HAS_OCIO` 分支换成 `return std::make_unique<OcioPipeline>(...)`，所有 callers 零改动。

`inline` + header-only：factory 自身没有 cpp TU，返回的 `std::unique_ptr<Pipeline>` 不带链接依赖。`#include "color/pipeline.hpp"` 一次即可。

**新增 `tests/test_color_pipeline.cpp`（12 个 suite 总数 → 12）.** 两 case：

1. `make_pipeline()` 返回非 null `Pipeline*`。
2. `IdentityPipeline::apply()` 在 bt709-limited → bt709-limited 的 identity case 返回 `ME_OK` + 不变 buffer + 空 err。

Factory 没有 consumer 时 inline 函数体**永远不被实例化**，preprocessor branch 和返回类型都躲过 `-Werror` 检查。加 test 就是强制至少一个 TU 实例化 factory——抓编译错误 / 头依赖漂移 / `#if ME_HAS_OCIO` 分支里的语法 regression。

**Alternatives considered.**

1. **工厂直接 `inline IdentityPipeline& default_pipeline()` 返回引用到静态单例**——拒：单例跨 thread 的 lifecycle / mutable 共享状态都是坑；`unique_ptr` 每次 new 是便宜的（Identity 是 empty class），消费方可持也可扔；OcioPipeline 一旦有 LUT cache 也是 per-instance 状态，不适合单例。
2. **把 factory 放 `src/color/pipeline.cpp`**（有 TU 而非 header-only inline）——拒：今天 factory body 是两行，加一个 TU 换一个 `include<memory>`，编译单元数增 1 没收益。等 factory body 吃 runtime config（比如根据环境变量选 pipeline）时再切 out-of-line。
3. **暴露 factory 给公共 C API**（`me_color_make_pipeline`）——拒：`me::color::Pipeline` 现在只是 internal 抽象，不走 C ABI；C 调用方 M2 还不存在；等 multi-track compose / frame-server / future orchestrator 的 internal consumer 先驱动 API shape。
4. **写单独 `me::color::Pipeline::create()` static method 而不是 free function**——拒：static method on abstract base class 把 factory 逻辑栓进 class hierarchy；free function 让未来加 `make_fallback_pipeline()` / `make_with_config(const Config&)` 等 factory variant 自由度更高。
5. **不加 test**——拒：inline factory + 无 consumer 意味着 header 语法 regression 滑过编译。2 case / 60 lines 的 smoke test 性价比极高。

业界共识来源：C++ "factory + abstract base + `std::unique_ptr` return" 是 Google style guide 和 Scott Meyers 的 *Effective Modern C++* Item 18 / 21 都推的 default pattern（比 `std::shared_ptr` 更轻，比 raw new 更安全）。inline header-only factory + `#if FEATURE` 分支形态也是 `fmt` lib、`spdlog`、OpenEXR 的 C++ API pattern。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean（头文件 instantiate 通过新 test 驱动）。
- `ctest --test-dir build` 12/12 suite 绿。
- `build/tests/test_color_pipeline` 两 case / 5 assertion / 0 fail。
- 不动 src/ 其他位置，其他 11 个 suite 继续绿。
- `-DME_WITH_OCIO=ON` 路径未重跑（上个 cycle 已证 OCIO 自己的 yaml-cpp 嵌套 CMake 在 4.x 下有 policy 问题——不是本 factory 代码的问题）。本 cycle 的 factory **不**吃 OCIO 符号，OFF 和 ON 两条路径都只 return IdentityPipeline，所以即使 OCIO fetch 将来成功，这条 factory 的 `#else` 路径行为不变。

**License impact.** 无新依赖。header-only + STL。

**Registration.** 无 C API / schema / kernel 变更。
- `src/color/pipeline.hpp`：新 inline `make_pipeline()` 函数 + `<memory>` include。
- `tests/test_color_pipeline.cpp` 新 suite + `tests/CMakeLists.txt` 登记 + `target_include_directories`。
