## 2026-04-23 — effect-registry-api-skeleton：Effect / EffectChain 内部抽象骨架（Milestone §M3 · Rubric §5.1 / §5.2）

**Context.** M3 exit criteria "EffectChain 能把连续 ≥ 2 个像素级 effect 合并成单 pass" + "≥ 3 个 GPU effect（blur / color-correct / LUT）" 的基础 API 骨架。先立 CPU 路径的 Effect interface + EffectChain 容器 + 1 个 no-op IdentityEffect，证明 end-to-end 可用；GPU 路径 (bgfx + shader 合并) 在 bgfx 集成 cycle 之后作 `effect-gpu-blur` / `effect-gpu-color-correct` / `effect-gpu-lut` 三个独立 bullet 填实装。

Before-state grep evidence：
- `grep -rn 'me_effect\|class Effect\|EffectChain' include src` 返回空（除 `src/task/task_kind.hpp:41` 的 `RenderEffectChainGpu = 0x3003` task-kind 占位符，无具体实装）。
- 无 `src/effect/` 目录。
- `docs/VISION.md` §3.2 的 "effect parameters are typed"（硬规则：不许 `Map<String, Float>`）—— 本 bullet 的 API 形状选择必须遵守。

**Decision.**

1. **`src/effect/effect.hpp`** —— `me::effect::Effect` 抽象类（virtual destructor + pure virtual `apply(rgba, W, H, stride)` + `kind()` 返 C-string debug tag）。pure header（no .cpp 需要）。
2. **`src/effect/identity_effect.hpp`** —— `IdentityEffect` final class，`apply` 空实现，`kind()` 返 `"identity"`。用于 end-to-end 测试 + 作为 chain-optimizer 的测试 fixture（"identity in middle elided" 场景）。
3. **`src/effect/effect_chain.hpp`** —— `EffectChain` class：
   - `std::vector<std::unique_ptr<Effect>> effects_`，move-only（拷贝禁止），size()/empty()/kind_at(i) 公开访问。
   - `append(std::unique_ptr<Effect>)`：move-in，null 断言 + silent no-op 兜底。
   - `apply(rgba, W, H, stride)`：per-effect sequential in-place 应用。future M3 exit criterion "EffectChain 能合并连续像素级 effect 单 pass" 会扩展这个 apply 到 "当后端支持 shader 合并时 → 合并 render"。CPU 初版保持最 naive。
4. **`tests/test_effect_chain.cpp`** —— 6 TEST_CASE / 42 assertion：
   - Empty chain → bytes 原样保留。
   - Identity alone → bytes 原样保留。
   - 两个 FillEffect（内部测试用）with different fills → 最后一个 fills dominant（declaration order）。
   - Fill + Identity + Fill → last Fill wins（identity 不扰动链）。
   - kind_at out-of-range → nullptr。
   - Move-only：move ctor 转移 ownership，src 变空，dst 保全功能（含 size() + kind_at + apply）。
5. **`src/effect/` 不进 media_engine library target** —— 全 header-only（.hpp only），未被任何 src/ cpp include。直接让测试通过 `target_include_directories(... src)` 使用。当 compose 层开始集成 EffectChain（例：ComposeSink 在每 track 的 RGBA → alpha_over 之前调 chain.apply()），再 bridge 到 src CMake。

6. **Public C API 暂不开**：bullet 原文提到 "C API `me_effect_kind_t` enum 占位 + typed params struct"。本 cycle 决定**不**推出 C ABI 直到第一个真实 GPU effect 确定参数形状。理由：
   - VISION §3.2 + CLAUDE.md 公共头硬规：任何 public header 一开即锁 ABI。`me_<effect>_params_t` struct 一出就不能改字段位置 / 字段语义，会付出多年维护税。
   - blur / color-correct / LUT 三个 effect 的具体参数（blur radius 是 rational? 是 int? horizontal-only flag? LUT 是内嵌 CUBE-format 还是 asset ref? color-correct 是 contrast/brightness 还是 tone curve?）今天都没定论。猜早了就锁死了。
   - 内部 abstraction 无 ABI 代价；等确定参数后再 graduate 到 C API。`docs/PAIN_POINTS.md` 已有 `me-output-spec-typed-codec-enum` 一条追类似 pain 的条目（codec spec 急用 string 后来补 enum 的债）。此处跳过早熟 API 规避同债。

7. **CPU-only 起步**：Effect interface 的 `apply` 签名现在是 RGBA8 in-place。GPU effects 未来会加 backend-specific 抽象（texture handle / shader program / uniform binding）——那时 Effect 基类要么长出 `apply_gpu(bgfx::FrameBufferHandle, ...)` overload，要么整个虚函数换成 tagged union (enum + payload)。本 cycle 不猜 GPU 形状。

**Alternatives considered.**

1. **tagged union 设计 (`Effect = std::variant<Identity, Blur, ...>`) 一步到位** —— 拒：variant 的前瞻性需要知道所有未来 effect。GPU 集成后可能引入 "编译时 effect" / "run-time plugin effect" 等分化，variant 反而僵。虚函数接口对未知未来最宽容；如果真需要 variant，数个 effect 后可无痛转。
2. **直接开 public C ABI** —— 拒（见上 §6）。
3. **把 Effect 做成纯函数指针 + 参数 struct pointer（C-friendly 风格）** —— 拒：C++ 虚函数接口对内部代码更自然；C ABI 等成熟后专门设计。
4. **完全 skip，先做 bgfx-integration-skeleton** —— 本 cycle pivot 掉 bgfx 的理由：bgfx FetchContent 本身是半天工程（bgfx 用非 CMake 构建系统，集成要么用 `bkaradzic/bgfx.cmake` wrapper，要么 vendor 源码），scope 超出单 cycle。Effect skeleton 不依赖 GPU，不受 bgfx 进度阻塞，先行落地为 GPU effect 做 scaffolding 更经济。
5. **IdentityEffect 写成 cpp 文件而非 header-only** —— 拒：零 LOC 实现体，header 里 inline 无成本。

**Scope 边界.** 本 cycle **交付**：
- `Effect` abstract interface。
- `IdentityEffect` no-op 实装。
- `EffectChain` 容器 + sequential apply。
- 6 单元测试涵盖 empty / identity / order / introspection / move。

本 cycle **不做**：
- Public C API (`me_effect_kind_t` / `me_*_params_t`)。
- 任何真实 effect（blur / color-correct / LUT）——独立 bullet。
- GPU backend 集成——`bgfx-integration-skeleton` 单独处理。
- ComposeSink 内接入 EffectChain——等 bgfx + 首 effect 落地后再 wire。
- EffectChain 的 shader-merge 优化器——M3 exit criterion 目标，先 CPU naive 走通再上 GPU 合并。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 28/28 suite 绿（+1 suite `test_effect_chain`）。
- `test_effect_chain` 6 case / 42 assertion。

**License impact.** 无（pure C++）。

**Registration.**
- `src/effect/effect.hpp`：新文件。
- `src/effect/identity_effect.hpp`：新文件。
- `src/effect/effect_chain.hpp`：新文件。
- `tests/test_effect_chain.cpp`：新文件。
- `tests/CMakeLists.txt`：新 test suite + include。
- `docs/BACKLOG.md`：**删除** `effect-registry-api-skeleton` bullet——sole scope 落地。
- **不**动 `bgfx-integration-skeleton` bullet（保留，下次 cycle 或专门处理）。

**§M 自动化影响.** M3 current milestone，5 exit criteria 均未打勾。本 cycle 是 M3 的第一个 feat commit，但没有单独 criterion 由此 tick —— effect 骨架是 "EffectChain 合并" + "≥3 GPU effect" 的**前置**，自身不是 criterion。§M.1 不 tick。
