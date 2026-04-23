## 2026-04-22 — me_probe 实装 (Milestone §M1 · Rubric §5.1 + §5.2)

**Context.** M1 exit criterion「`me_probe` 实装」是 BACKLOG 的第一条 P0。旧 `src/api/probe.cpp`
全部返回 `ME_E_UNSUPPORTED`，`me_media_info_*` accessor 返回零值/空串。引擎里已有 FFmpeg
链（`FFMPEG::avformat / avcodec / avutil`），`src/io/demux_kernel.cpp` 展示了 `avformat_open_input`
+ `avformat_find_stream_info` 的正确握手，probe 是 M1 首批把引擎 FFmpeg 依赖用出去的
API——不要求解码帧，只读元数据，走软件路径最便宜。

**Decision.**
- 把 `struct me_media_info` 的具体定义放进 `src/api/probe.cpp`（opaque 对外，POD-style 内部，
  `std::string` 持有字符串，me_rational_t 持有时间/帧率，bool + int 持有拓扑/维度）。
  公共 header `include/media_engine/probe.h` 未动——只有 impl 变了，ABI / 声明全保持原样。
- `me_probe` 流程：strip `file://` → `avformat_open_input` → `avformat_find_stream_info` →
  `av_find_best_stream(AVMEDIA_TYPE_VIDEO/AUDIO)` → 填充 container / duration / W×H /
  video codec / `av_guess_frame_rate` → audio sample_rate / `ch_layout.nb_channels` /
  audio codec → `avformat_close_input`。失败路径显式 `me::detail::set_error` 写 FFmpeg
  诊断串。分配失败走 `std::bad_alloc` 捕获翻译成 `ME_E_OUT_OF_MEMORY`；其他 `std::exception`
  翻译成 `ME_E_INTERNAL`——C API 边界零 C++ 异常逸出（CLAUDE.md invariant 1）。
- Container name 取 `fmt->iformat->name` 的第一个逗号前 token——FFmpeg 的 MOV/MP4 demuxer
  暴露成 `"mov,mp4,m4a,3gp,3g2,mj2"`，按 FFmpeg 惯例「短名就是第一个 token」，其它
  caller（ffprobe `-show_format`）也是同样输出。
- Duration 采用 FFmpeg 的 AV_TIME_BASE 单位（1/1 000 000 秒），直接构造成
  `me_rational_t{fmt->duration, AV_TIME_BASE}`，不做化简——callers 需要化简自己调用
  `av_reduce`/等价路径，保留 `AV_TIME_BASE` 语义更便于追踪来源。`AV_NOPTS_VALUE` → `{0, 1}`。
- Frame rate 走 `av_guess_frame_rate(fmt, stream, nullptr)`——不是 `r_frame_rate`（上界）
  也不是 `avg_frame_rate`（对 VFR 不稳定），FFmpeg 自己的 API 文档明确 `av_guess_frame_rate`
  为「现代默认选择」，与 ffprobe 内部逻辑一致。
- Audio channels 用 `AVCodecParameters::ch_layout.nb_channels`（FFmpeg ≥ 5.1），不再
  fallback `legacy channels`——本仓 CLAUDE.md 规定 Homebrew FFmpeg 为开发依赖，目前是
  8.x，legacy 字段已 deprecated。
- 新增 `examples/04_probe/main.c` 作为端到端 smoke 样例 + C-only 编译边界验证（CLAUDE.md
  invariant 1 要求「公共头必须用 C 编译通过」——新 example 用 `.c` 编，且不引用 media_engine
  内部 symbol，任何 C++ types 泄露到 header 就会立刻编译失败）。
- 预置一次 `fix(cmake): use Taskflow target name`（独立 commit）——
  `src/CMakeLists.txt` 和 `examples/02_graph_smoke/CMakeLists.txt` 之前链 `Taskflow::Taskflow`，
  但 `FetchContent_MakeAvailable` 创建的是 `Taskflow`（Taskflow 自家 CMakeLists.txt 第 301 行
  `add_library(${PROJECT_NAME} INTERFACE)`，`::` 命名空间 alias 只在 `find_package(Taskflow CONFIG)`
  安装后才存在）。无此修复，`cmake -B build` 直接在 Generate 阶段失败，iterate-gap 无法跑
  任何验证——视为 probe-impl cycle 的前置，独立成 commit 便于事后追溯。

**Alternatives considered.**
- **一次 `avio_open` 自己读头再拆包**：比 `avformat_open_input` 便宜（不启动 demuxer 状态机），
  但也意味着重新实现 container sniff/timestamp/stream 枚举的逻辑。FFmpeg probe 是其他所有
  播放器（VLC / mpv / ffprobe）直接使用的路径，没有理由自己再写一遍；M1 的目标是「让
  libavformat 跑起来」，不是自研解析器。拒。
- **把 container name 存原始 `"mov,mp4,m4a,3gp,3g2,mj2"`**：对 caller 最诚实，但
  schema / 未来 container 白名单对比都得先 split 一遍——转发到 caller 是 push complexity。
  拒。
- **`codecpar->codec_name` 替代 `avcodec_get_name(codec_id)`**：FFmpeg 8.x 保留了
  `codec_name` 字段但标注「可能为空」，对 stream 没有 `extradata` 的场景经常 empty
  string。`avcodec_get_name(codec_id)` 走 codec 注册表，永远返回有效常量。拒。
- **把 `me_media_info` 结构声明进公共 header**：能让 host 语言 FFI 绑一份 POD struct
  而不是走 accessor，但公共 header 就得 `#include <stdint.h>` 以外的东西（或自己把
  `std::string` 的尺寸/布局暴露出去）。目前 API.md 明确把 `me_media_info_t` 列为 opaque
  handle，保持 accessor 抽象未来换内部存储（比如复用 DemuxContext 缓存）可以 ABI-silent。
  拒。
- **audio `channel_layout` 暴露成一个独立字段（`uint64_t`）**：某些 host 下游确实要它
  做 downmix 决策，但 M1 规划里的最小集合只有 sample_rate + channels，暴露再多字段都要
  考虑 ABI-stable 表示（FFmpeg 也刚从 `uint64_t channel_layout` 迁到
  `AVChannelLayout` 结构），现在定型代价高。延后到 M4 音频 polish。拒。

**Coverage.**
- `examples/04_probe/04_probe /tmp/input.mp4` — 打印 container / duration / 视频维度
  与帧率 / 音频 sample rate + 声道 + codec，格式符合预期（对一个 2s 1920x1080@30 + aac
  44100Hz 单声道的 MP4：container=mov, duration=2000000/1000000, video=h264 1920x1080
  @30/1, audio=aac 44100Hz 1ch）。
- `examples/04_probe/04_probe file:///tmp/input.mp4` — 相同输出，file-scheme 剥离正确。
- `examples/04_probe/04_probe /tmp/nope.mp4` — 返回 `ME_E_IO`，`me_engine_last_error`
  带 `avformat_open_input: No such file or directory`。
- `examples/01_passthrough` 对现有 `sample.timeline.json` + `/tmp/input.mp4` 仍产出合法
  MP4（H.264 1920x1080 @ 30/1, yuv444p, 30 frames），ffprobe 无 warning / error。
- `clang -xc -std=c11 -fsyntax-only -Iinclude -` 喂含 `me_probe` + 所有 accessor 调用
  的最小 C TU，编译通过——`me_media_info_t`、`me_rational_t`、`me_status_t` 等类型在
  C 客户端均可用。
- `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON` 完整重建通过，
  `-Wall -Wextra -Werror` clean。

**License impact.** 无新依赖。`libavformat/libavcodec/libavutil` 在链图里已存在
（LGPL，见 ARCHITECTURE.md 白名单）。新增的 example `.c` 文件只链 `media_engine` 公共 API。

**Registration.** 本轮变动的注册点：
- `TaskKindId` / kernel registry: 未动。
- `CodecPool` / `FramePool` / resource factory: 未动（probe 不走 pool，直接开 AVFormatContext 局部用一次就关）。
- Orchestrator factory: 未动。
- 新导出的 C API 函数: 未新增（仅从 `ME_E_UNSUPPORTED` stub 实装为真 impl；符号表不变）。
- CMake target / install export / FetchContent_Declare: 未动（修的是 `Taskflow` target 名，`FetchContent_Declare` 本身未改）。
- JSON schema 新字段 / 新 effect kind / 新 codec 名: 未动。
- 新 CMake target: `examples/04_probe/04_probe`（本地验证 + 建议 CI smoke）。
