## 2026-04-23 — me-probe-more-fields：扩 6 个视频 probe accessor（Milestone §M2-prep · Rubric §5.1）

**Context.** `me_probe` / `me_media_info_*` 在 M1 只回 container / codec / duration / W×H / 帧率 / sample_rate / channels——够 M1 exit 但 M2 的多轨合成 + OCIO 集成需要更多：`rotation`（iOS 竖屏视频普遍带 90° 旋转的 display matrix，不读直接合成会把人像躺着合）、`color_range`（limited 16-235 vs full 0-255，算错直接导致整片暗一档或过曝一档）、`color_primaries / transfer / matrix`（OCIO 工作空间转换决定 BT.709 和 BT.2020 是否需要色域 mapping；transfer 关键决定 PQ/HLG HDR 是否进入 linear 前正确 EOTF）、`bit_depth`（8-bit SDR 与 10-bit HDR10 Main10 要在 pipeline 早期分叉）。这些字段 libavformat 都已经从 codecpar / 容器元数据拿到，但 C API 这一层没暴露出来。本轮在不改 ABI 既有签名的前提下，把这 6 个字段以 append-only 方式加到 `include/media_engine/probe.h`。

**Decision.** 在 `include/media_engine/probe.h` 末尾 append 6 个新 accessor（保留所有现有函数签名和 struct 顺序不变）：

- `int me_media_info_video_rotation(info)` — 度数，规范化到 {0, 90, 180, 270}；从 `cp->coded_side_data` 里拿 `AV_PKT_DATA_DISPLAYMATRIX`，`av_display_rotation_get()` 返回 CCW 角度，取负转 CW 再按 90° 吸附（超出 90° 倍数的矩阵算容器损坏，返回 0）。
- `const char* me_media_info_video_color_range(info)` — `av_color_range_name(cp->color_range)`（`"tv"` / `"pc"` / `"unspecified"` / `"unknown"` 等）。
- `const char* me_media_info_video_color_primaries(info)` — `av_color_primaries_name(cp->color_primaries)`。
- `const char* me_media_info_video_color_transfer(info)` — `av_color_transfer_name(cp->color_trc)`。
- `const char* me_media_info_video_color_space(info)` — `av_color_space_name(cp->color_space)`（YCbCr→RGB 矩阵；类型名 "color_space" 贴合 libav，文档里点出它是"matrix"的别名）。
- `int me_media_info_video_bit_depth(info)` — `av_pix_fmt_desc_get(cp->format)->comp[0].depth`，未知 pix_fmt 返回 0。

实现侧：`struct me_media_info`（`src/api/probe.cpp`，opaque，**不是** public struct——body 改动对 ABI 不可见）末尾 append 对应字段，`me_probe` 内 video stream 分支里填。`av_*_name()` 可能返回 `nullptr`（未识别 enum），用 `name_or_empty()` helper 统一翻译成空 `std::string`——不允许裸 `nullptr` 通过 accessor 出去，不然就要求调用侧 `NULL` 检查，破坏"accessor 总是返回有效 `const char*`" 的既有约定（见现有 `video_codec` 等字段同模式）。

`examples/04_probe/main.c` 加 6 行打印，exec 这一层也覆盖了这些 accessor 从 C 代码调用的路径。C-only header 编译通过 `clang -xc -std=c11 -fsyntax-only -Iinclude` 验证。

**ABI 分析.** 严格 append-only：
- 现有 C 函数签名全部保留。
- 新增函数放在头文件末尾，extern "C" 块之内，放在 `#include "types.h"` 后（保持 types.h 单一依赖不变）。
- `me_media_info` 是 opaque 类型（C API 只通过 accessor 访问），struct body 可自由扩展而不破坏 ABI。
- 旧 header 编出的客户端 binary 继续 link 到新 `libmedia_engine.a`：符号表是 super-set，没有符号消失。
- 新 header 编出的客户端 binary **不能**回落到旧 `libmedia_engine.a`：新 6 个符号找不到。这是 append-only 的正常约束，不算破坏。

**Alternatives considered.**

1. **直接暴露 `me_media_info` struct body**（non-opaque）让调用方按字段名访问——拒：违反 CLAUDE.md invariant #1 "opaque handles"；而且 struct body 变化会直接破坏调用方的 struct size，ABI 演进成本高。
2. **用一个聚合 getter `me_media_info_video_color(info, me_color_info_t*)`**——看似"一次 call 拿一整组"更整洁，但要先引入 public struct `me_color_info_t`（POD），未来加 mastering display / MaxCLL / MaxFALL 会要末尾 append 新字段，又一次 ABI 审查。分散 accessor 更简单、更稳——每个字段独立演进。
3. **返回 `int` enum + `me_color_range_t` / `me_color_primaries_t` 公共 enum**——类型安全更好，但 enum 值一旦发布就冻结（invariant：不能改 enum 值），未来 FFmpeg 引入新 primaries 就要自己加 enum 条目，维护负担。字符串 passthrough + 定期在 decision 里记录 FFmpeg 版本约定，成本更低；ffprobe 生态也都是字符串。
4. **把 `video_rotation` 返回 `me_rational_t`** 表示任意角度——拒：VISION §3.2 要求 typed 表达，但"旋转"在容器元数据里规范只允许 90° 倍数；暴露任意角度反而邀请调用方处理永远不会真实出现的 case。Int degrees 更贴合真实输入分布。

业界共识来源：ffprobe `--show_streams` / MediaInfo Template 都是按独立字段回 rotation / color_range / primaries / transfer / space——字符串形态且 append-only；FFmpeg 自己的 `av_*_name()` 家族也都是 const char* 字符串（`libavutil/pixdesc.c`），我们用相同蕴涵。

**Coverage.**

- `cmake --build build` 与 `-Werror` 全过，`ctest --test-dir build` 7/7 绿。
- `clang -xc -std=c11 -fsyntax-only -Iinclude` 喂最小 C 用例调用 3 个新 accessor 通过——extern "C" boundary 干净。
- `04_probe build/tests/fixtures/determinism_input.mp4`（无 color tag 的 libav BITEXACT 编码）：rotation=0, bit_depth=8, 其他 "unknown"。
- `04_probe /tmp/h264tagged.mp4`（`ffmpeg -colorspace bt709 -color_range tv` 显式打 tag）：color_range=tv, color_space=bt709，和 `ffprobe --show_streams` 完全一致。
- 01_passthrough 端到端未动，回归绿。

未加 doctest 直接覆盖新 accessor——本轮 scope 是 ABI 扩；端到端 `04_probe` 覆盖了"accessor 从 C 能调"的核心契约；专属 unit test 留作后续 debt（probe 自身还没有 test_probe.cpp，属于独立 gap）。

**License impact.** 无新依赖。`libavutil/display.h` + `libavutil/pixdesc.h` + `libavcodec/packet.h` 都是已链入的 FFmpeg 组件。

**Registration.** C API 新增 6 个导出函数：
- `me_media_info_video_rotation`
- `me_media_info_video_color_range`
- `me_media_info_video_color_primaries`
- `me_media_info_video_color_transfer`
- `me_media_info_video_color_space`
- `me_media_info_video_bit_depth`

Opaque struct `me_media_info` 内部 body 扩 6 个字段（ABI 不可见）。无新 CMake target / FetchContent / JSON schema / kernel 注册 / effect kind。
