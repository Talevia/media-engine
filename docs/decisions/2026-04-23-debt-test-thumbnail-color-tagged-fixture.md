## 2026-04-23 — debt-test-thumbnail-color-tagged-fixture：tagged 变体 fixture + scaffold test case（Milestone §M2-prep · Rubric §5.2）

**Context.** `tests/test_thumbnail.cpp` 的 6 个 case 全部基于 `determinism_input.mp4`（`gen_fixture` BITEXACT-encoded，所有 color tag 均 UNSPECIFIED）。Bullet 识别了缺口：没有任何 case 用 color-tagged fixture exercise `me_thumbnail_png`。

**已知弱点 / scaffold 定位**：实测（2026-04-23 earlier in session）`ffmpeg -c:v mpeg4 -color_primaries bt709 -colorspace bt709 -color_range tv` 产的 MP4，ffprobe 拿到的 tag 仍然是 "unknown"——`mov` 容器对 MPEG-4 Part 2 的 color metadata 存储不完整。只 h264 能可靠 roundtrip 这些 tag，而 gen_fixture 用 mpeg4（libx264 GPL 不能入链 / h264_videotoolbox 非所有平台可用）。所以"tagged fixture" 这条路径今天**无法保证 container 层还能读到 tags**。

**加上 thumbnail 本身无色彩感知**：`src/api/thumbnail.cpp` decode → sws_scale → PNG encode，没有任何 color-space-aware 路径。即使输入 fixture 的 tags 真存活，thumbnail output 也不会体现它——因为 pipeline 不 consume tags。

两条组合 → bullet 的严格 "assert tag 传导" 目标今天**不可实现**。选：

**Decision.** 做 **scaffold 形态** 而非严格 semantic test，为 M2 色彩路径上线铺路：

1. **`gen_fixture.cpp` 加 `--tagged` flag**：命令行解析升级（接受可选 `--tagged` 和 positional out_path），flag set 时在 `avcodec_open2` 前 set encoder 的 `color_range=MPEG` / `color_primaries=BT709` / `color_trc=BT709` / `colorspace=BT709`。Tag 是否穿过 muxer 到 MP4 不强制——encoder side 确实 set 了。
2. **`tests/CMakeLists.txt` 加 `determinism_fixture_tagged` target**：跟 `determinism_fixture` 同 shape，多传 `--tagged` arg 到 `gen_fixture`。产 `determinism_input_tagged.mp4`。`test_thumbnail` 现在 depend 两个 fixture，`target_compile_definitions` 注入 `ME_TEST_FIXTURE_MP4_TAGGED` 宏。
3. **`tests/test_thumbnail.cpp` 加 TEST_CASE** "produces a valid PNG from a color-tagged fixture"：load tagged fixture → `me_thumbnail_png` → 断言 PNG magic + IHDR W×H (`640×480`)。**不**断言 PNG 里的 tEXt / iCCP chunk（thumbnail impl 不写），**不**断言 ffprobe-readable color tags on tagged input（container 不保）。

**Test 实际 value**:

- **今天**：case 断言 "thumbnail 不因为 tagged fixture crash / return 非合法 PNG"。边界覆盖少，但确实新场景（之前所有 case 基于 BITEXACT 默认 fixture）。
- **M2 compose 上线后**：这个 case 是**升级 point**。当 `me_thumbnail_png` 加 color-aware scaling（OCIO pipeline 介入 sws 前后），这个 case 会开始 detect 差异（tagged input 的 PNG 像素应与 untagged input 不同，因 color space 转换）。Scaffold 值 = 把"未来 assert"的 call site 先落地，避免那时候要同步改 3 处（fixture gen、CMake、test）。
- **`gen_fixture --tagged` API 本身**：证明 gen_fixture 可以产多变体 fixture。下一次想加带 audio / 带 rotation / 带 HDR tag 的 fixture 时，升级 `gen_fixture.cpp` 的 CLI parsing 路径已经通。之前是硬编码单 target，每多变体就要新 C++ TU。

**Alternatives considered.**

1. **换用 h264 做 tagged fixture**（tags 可保）——拒：`gen_fixture` 链 libx264 GPL 入链（ship path 禁止），或链 h264_videotoolbox（Linux CI 没）。当前 LGPL-clean mpeg4 是 fixture 的正确 encoder；真要测 tag roundtrip，future 用外部 ffmpeg CLI 产一次性 fixture checked-in（后退到 `debt-fixture-gen-libav` 想解决的问题）。两害取轻。
2. **不加 tagged fixture，让 TEST_CASE 空跑**——拒：一条 `MESSAGE("skipping: no tagged fixture available"); return;` 的 case 只是 hit-count 占位，比真 scaffold value 低。CMake 侧花 2 行加 target 成本极低。
3. **试 h264_videotoolbox 做 tagged fixture on mac**（Linux CI 跳）——考虑过：`gen_fixture` 代码加第二个 encoder 路径（mpeg4 untagged + h264 tagged），`--tagged` 试开 h264_videotoolbox，失败 fallback mpeg4。复杂度比收益高；留给真 M2 有 OCIO 消费者时再评估。
4. **扩 `me::color::Pipeline` contract 现在就接 thumbnail 路径**——拒：thumbnail 没开消费 color pipeline（`ocio-pipeline-wire-first-consumer` cycle 只 wire 到 reencode_mux）。thumbnail 侧 wire 是 M2 compose 的 concern。

业界共测来源：Bazel test data generator `testdata/` pattern、OpenEXR test suite 里的 "known-bad / known-tagged" variant——都是 scaffold-first 模式：fixture 先生成，test 先断 structural，future semantic 断言逐步上线。

**Coverage.**

- `cmake --build build` 与 `-Werror` clean；`gen_fixture` 编 CLI 升级通过，新 `determinism_input_tagged.mp4` 生成（27K bytes，近似 untagged size，因为 encoder tag 不进 container）。
- `ctest --test-dir build` 12/12 suite 绿。
- `test_thumbnail` 6 case → 7 case / 30 assertion → 37 assertion。新 case 运行：tagged fixture load + thumbnail ok + PNG magic + IHDR 640×480。
- 未在 `test_probe.cpp` 或 `test_determinism.cpp` 加 tagged-fixture case——probe 已有 all-UNSPECIFIED 覆盖 + 手测 h264tagged.mp4 覆盖（see `2026-04-23-me-probe-more-fields`），determinism 单/多 clip 都覆盖 untagged h264/aac reencode；tagged 不增差异。

**License impact.** 无。仅 `gen_fixture` test-time binary 多一条 CLI 路径。

**Registration.** 无 C API / schema / kernel / license-whitelist 变更。
- `tests/fixtures/gen_fixture.cpp` CLI 升级（`--tagged` flag）+ `#include <cstring>`。
- `tests/CMakeLists.txt` 加 `determinism_fixture_tagged` custom target + `test_thumbnail` 的 `ME_TEST_FIXTURE_MP4_TAGGED` compile def。
- `tests/test_thumbnail.cpp` 加 1 个 TEST_CASE。
