## 2026-04-23 — debt-split-timeline-loader-cpp：提取 parse helpers（Milestone §M2 debt · Rubric 外）

**Context.** `src/timeline/timeline_loader.cpp` 476 行（P1 debt 区间 400-700）。单 TU 包含 load_json 主体 + `LoadError` 结构 + 四 enum 转换 shim（to_primaries / to_transfer / to_matrix / to_range）+ `as_rational` / `require` / `rational_eq` 通用 util + 三块 shape parsers（parse_animated_static_number / parse_transform / parse_color_space）。

Before-state：
- `wc -l src/timeline/timeline_loader.cpp` = 476。
- Helper 函数占 lines 15-152（137 行，包括注释 + LoadError struct）。
- Main `load_json` 在 lines 157-476（320 行）。
- Helper 全部是 **纯数据形状解析**，无共享状态、无 load_json 的 local context。天然可抽离。

**Decision.**

1. **`src/timeline/loader_helpers.{hpp,cpp}`** 新文件：
   - Namespace `me::timeline_loader_detail`（明确 internal/implementation，非 public API）。
   - `struct LoadError {status, message}` + `as_rational` / `require` / `rational_eq`。
   - ColorSpace 字符串 → enum shims：`to_primaries`/`to_transfer`/`to_matrix`/`to_range`。
   - Shape parsers：`parse_animated_static_number`（仍拒 `keyframes` 形式，等 `transform-animated-support` loader 层 bullet 接手） / `parse_transform` / `parse_color_space`。
   - 所有函数在 .hpp 声明，.cpp 实装——无 inline 膨胀、无 header-only 把 nlohmann/json 暴露给非-loader 模块的风险。

2. **`src/timeline/timeline_loader.cpp`** 删除被移走的 137 行 + 加 `#include "timeline/loader_helpers.hpp"` + 内部 anon namespace 顶加 `using namespace me::timeline_loader_detail;`：
   - `using namespace` **在 anonymous namespace 内**——仅本 TU 受影响，不影响整个 `me::timeline` 命名空间的 load_json 使用者（实际 load_json 本身已在 `namespace me::timeline` 里，using 声明在更内层 anon namespace 中）。
   - 现有的 `require(...)` / `as_rational(...)` / `parse_color_space(...)` 等 call sites 通过 using-resolution 找到 helpers namespace 里的符号，无需加 `me::timeline_loader_detail::` 前缀 —— 零 call-site 改动。

3. **`src/CMakeLists.txt`** +`timeline/loader_helpers.cpp` 到 media_engine sources。

4. **结果**：
   - `timeline_loader.cpp`：476 → **343** 行（-133，-28%）。
   - `loader_helpers.cpp` 新：134 行。
   - 两文件合计 477 行 ≈ 原 476（净 +1 来自新文件的 doc-comment header）。行为字节等价。

5. **测试**：无新增。**所有现有测试作为回归覆盖**：
   - `test_timeline_schema` 47 case（含 positive parse / 多种 negative rejection 含 `colorSpace.primaries` 未知值 / gainDb on video clip 拒 / 等）——exercises 被移走的所有 helpers。
   - `test_engine`, `test_compose_sink_e2e`, `test_determinism` 等所有用 `me_timeline_load_json` 的测试——间接覆盖 parse helpers。
   - 27/27 -> 29/29 ctest 全绿。

**Alternatives considered.**

1. **Helpers 直接 inline 在 .hpp** —— 拒：parse_transform 里用 `std::string_view[]` + nlohmann/json 展开，inline 进 header 会增加任何 include 此 header 的 TU 的编译时间。.cpp 隔离编译，不引入跨 TU 压力。
2. **Helpers 以 `namespace me::timeline::detail`** —— 拒：后者容易和 `me::detail::` 或 `me::timeline::` 里的其它代码冲突。`me::timeline_loader_detail` 明确标出 "仅 loader 私有"。
3. **Helpers 全部 move 成 static class methods（`class LoaderHelpers { static double parse_...; }`)** —— 拒：自由函数更简洁；无状态类只是 Java-ism 架子。
4. **同 cycle 再拆 load_json 主体** —— 拒：load_json 现在 343 行，单 function 是大；但拆它需要把 per-section parse（assets / tracks / clips / transitions / output）各自成 helper + shared state struct，scope 大。单独 bullet `debt-split-load-json-body` 如果未来 >500 行再触发。
5. **把 `LoadError` 换成 `std::expected<T, me_status_t>` 或 `std::pair<me_status_t, std::string>`** —— 拒：throw-style 让嵌套 parse helper 代码简洁（直接 `require(...)` 抛，不用 chain error codes）；代码已依赖异常机制，改造 scope 大 + C++23 `std::expected` 需升 standard。本 cycle scope 外。

**Scope 边界.** 本 cycle **交付**：
- Helpers 提到独立 TU。
- timeline_loader.cpp 降 28%。

本 cycle **不做**：
- load_json 主体拆分。
- 把 LoadError 换成非异常机制。
- parse_animated_static_number 的 keyframes 支持（那是 `transform-animated-support` 的 loader 层 bullet）。

Bullet `debt-split-timeline-loader-cpp` **删除**——scope 已充分达成（476 → 343，远离 400 阈值）。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 29/29 suite 绿。
- `test_timeline_schema` 47 case 全绿（含所有 helper 间接覆盖）。
- timeline_loader.cpp 343 行（从 476）；loader_helpers.cpp 134 行（新）。

**License impact.** 无。

**Registration.**
- `src/timeline/loader_helpers.hpp/cpp`：新文件，`me::timeline_loader_detail` namespace。
- `src/timeline/timeline_loader.cpp`：移除 137 行 helper 定义 + 新 `#include "timeline/loader_helpers.hpp"` + anon-namespace 内 `using namespace ...`。
- `src/CMakeLists.txt`：+ `timeline/loader_helpers.cpp`。
- `docs/BACKLOG.md`：**删除** bullet `debt-split-timeline-loader-cpp`。

**§M 自动化影响.** M3 current milestone，debt refactor 不解锁 criterion。§M.1 不 tick。
