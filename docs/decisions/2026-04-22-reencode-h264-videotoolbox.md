## 2026-04-22 — reencode-h264-videotoolbox (Milestone §M1 · Rubric §5.6)

**Context.** M1 exit criterion「`me_render_start` 新增至少 1 条 re-encode 路径」。
之前 Exporter 只接受 `video_codec="passthrough" + audio_codec="passthrough"`，
所有其它 spec 走 `ME_E_UNSUPPORTED`。passthrough 是 stream-copy，本身不验证编码器
注册 / 格式转换 / encoder flush 语义——引擎对 encoder 这条路径零经验值。另一方面
VISION §3.4 禁 GPL，libx264/libx265 直接出局；Mac 上唯一免费 LGPL-clean 的 h264
编码器是 `h264_videotoolbox`，Homebrew FFmpeg 启了 `--enable-videotoolbox`。

**Decision.**
- 新文件 `src/orchestrator/reencode_pipeline.{hpp,cpp}` 对称于 `muxer_state.{hpp,cpp}`：
  暴露 `ReencodeOptions` + `reencode_mux(DemuxContext&, ReencodeOptions&, err*)` 单入口。
  Exporter 保留 graph-demuxer 前置，只在 worker thread 里依 spec 分派 `passthrough_mux` /
  `reencode_mux`——两者共用同一个 DemuxContext 和 progress lambda。
- 视频：`h264_videotoolbox` 找不到 → `ME_E_UNSUPPORTED` + 诊断串。encoder ctx 时基与输入
  stream time_base 对齐（1:1 PTS rescale），pix_fmt 统一转 `AV_PIX_FMT_NV12`（VideoToolbox
  原生 surface 格式），输入不是 NV12 就起 `sws_scale` 做 bilinear 转换；NV12 就直通。
- 音频：`aac`（libavcodec 内置，LGPL）。AAC 编码器强制 `AV_SAMPLE_FMT_FLTP`，sample_rate
  不在 MPEG-4 AAC 合法集（7350/8/11/12/16/22/24/32/44.1/48/64/88.2/96 kHz）时 clamp 到 48000。
  输入 → `swr_convert` → `AVAudioFifo` 缓冲 → 按 `enc->frame_size` chunk 喂 encoder。
- **Global header flag 在 open 前**：MP4/MOV 有 `AVFMT_GLOBALHEADER`，extradata 必须写进
  container 的 `avcC` / ESDS box 而不是 prefix 每个 key frame。`AV_CODEC_FLAG_GLOBAL_HEADER`
  必须在 `avcodec_open2` 之前 set 到 `codec_ctx->flags`——初版漏了这步，产物 ffprobe
  报 "No start code is found / Invalid data"，排查后 `open_video_encoder` /
  `open_audio_encoder` 新增 `bool global_header` 形参，Exporter 读 `oformat->flags &
  AVFMT_GLOBALHEADER` 传入。
- **Flush 链**：源 EOF 后先给 decoder 发 `avcodec_send_packet(nullptr)`，drain 出缓存帧送
  encoder；然后给 encoder 发 `avcodec_send_frame(nullptr)`，drain 出缓存包写入；音频额外
  在 drain 之前把 FIFO 里残留样本喂完（`drain_audio_fifo(true)` 把最后一个不满
  `frame_size` 的 chunk 也送出去）。这是 FFmpeg `doc/examples/transcode_aac.c` 的标准
  pattern，三级缓冲都必须走完才能保证尾部帧不丢。
- FindFFMPEG.cmake 新增 `swscale + swresample` 到 `_FFMPEG_REQUIRED_COMPONENTS`，
  `src/CMakeLists.txt` 链 `FFMPEG::swscale + FFMPEG::swresample`。FFmpeg 安装默认都带，
  视为必需组件不加选项。
- `Exporter::export_to` 的 spec 校验新增 `is_h264_aac_spec` 辅助，支持的两种合法 spec：
  `(passthrough, passthrough)` 和 `(h264, aac)`。其它组合返回 `ME_E_UNSUPPORTED`，错误
  信息显式列出支持的两种。
- `examples/05_reencode/` 新建，和 01_passthrough 结构完全对称——只改 video_codec /
  audio_codec 两个字段，其余 C 流程不变。

**Alternatives considered.**
- **libx264 + libfdk_aac**：编码质量最佳但前者 GPL（VISION §3.4 红线）、后者非标准 LGPL
  且 FFmpeg 官方 warning "not redistributable"。一撞红线直接拒。
- **`h264_videotoolbox` 用 BGRA 输入让 VideoToolbox 自己转**：能让 encoder 多一层
  Metal 支持且跳过 sws_scale；但引入 CPU/GPU memory copy + pix_fmt 在 Release-mode
  Homebrew 构建下不总是启用 Metal——引擎确定性路径上能不靠 GPU 就不靠。拒。
- **`aac_at`（AudioToolbox）替代内置 aac**：Mac 原生，质量更好，bit-exact per run
  但跨 OS 不可用，而 M1 当前 fallback 路径是 CPU。保留给 M4 audio polish，现在用
  `aac`（fdk-aac 的 LGPL 替代）。拒。
- **让每个 codec 独占一个 orchestrator entrypoint（`h264_exporter.cpp` /
  `prores_exporter.cpp` / ...）**：分支清晰但每次加 codec 都要重复写 demuxer
  preamble + 进度回调 + cancel 轮询。现在两个 spec 共享同一个 worker thread + 同一
  DemuxContext + 同一 progress cb pattern，下次加 HEVC 只改 `reencode_pipeline.cpp`
  里的 encoder 选择。拒——留作 M3+ codec 数量 >3 时再拆。
- **Graph 里新增 `VideoEncodeKind` / `AudioEncodeKind` kernel**：per-frame pure 约束
  与 encoder 的 stateful / delayed-output 本质冲突（encoder 内部有 GOP 缓冲 / lookahead，
  一个 input frame 不总对应一个 output packet）。graph node 只适合 "N 输入 → 1 输出
  的 pure 变换"（filter / color-convert）。batch encode 固守 orchestrator 侧 stateful
  object。架构文档 §批编码 已经承认这点。

**Coverage.**
- `./build/examples/05_reencode/05_reencode examples/01_passthrough/sample.timeline.json /tmp/reencoded.mp4`
  在 2s 1920x1080@30 / aac 44.1 kHz 单声道源上跑完：container=mp4, video=h264
  1920x1080 @ 30/1, audio=aac 44100 Hz 1 ch, duration=2.02s（AAC 编码器 priming 延迟
  正常）。`ffprobe -v error` 0 error。
- `./build/examples/01_passthrough/01_passthrough ... /tmp/out.mp4` 回归仍产合法 MP4。
- `./build/examples/04_probe/04_probe /tmp/reencoded.mp4` 正确识别 h264 + aac。
- `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DME_WERROR=ON` 完整构建通过，
  `-Wall -Wextra -Werror -Wpedantic` clean。
- `clang -xc -std=c11 -fsyntax-only -Iinclude -` C 客户端最小 TU（05_reencode/main.c）
  编译通过——C API 没泄露 C++ 类型。

**License impact.** 无新 FetchContent。`FFMPEG::swscale` + `FFMPEG::swresample` 都是
LGPL，和 `FFMPEG::avformat/avcodec/avutil` 同 license（ARCHITECTURE.md 白名单已收录
"FFmpeg LGPL"）。`h264_videotoolbox` 依赖 macOS VideoToolbox framework（系统 proprietary
但属于 OS API 调用，跨 license 边界合规——等同于用 CoreFoundation）。`aac` 编码器
是 libavcodec 内置（LGPL）。

**Registration.** 本轮变动的注册点：
- `TaskKindId` / kernel registry: 未动（reencode_mux 不是 Task，是 orchestrator 侧
  stateful object——符合 ARCHITECTURE_GRAPH.md §批编码 的架构）。
- `CodecPool` / `FramePool` / resource factory: 未动（M1 bootstrap CodecPool 还是空壳，
  这里直接用局部 AVCodecContext unique_ptr；迁到 pool 是 M4+ 的 `codec-pool-impl` 轴）。
- Orchestrator factory: 未动（仍是 `Exporter` 单类，内部分派）。
- 新导出的 C API 函数: 未新增（`me_render_start` 签名不变，只是新 spec 组合从
  `ME_E_UNSUPPORTED` → `ME_OK`）。符号表稳定。
- CMake target / install export / FetchContent_Declare: `FindFFMPEG.cmake` 新增两个
  组件（swscale + swresample）；不是 FetchContent，系统查找；`src/CMakeLists.txt`
  `target_link_libraries` 新增两项。`examples/CMakeLists.txt` 新增子目录
  `05_reencode`。
- JSON schema 新字段 / 新 effect kind / 新 codec 名: `me_output_spec_t.video_codec`
  新增合法值 `"h264"`，`audio_codec` 新增 `"aac"`——schema 结构本身未动，只是接受域
  扩大。
- 新 source 文件: `src/orchestrator/reencode_pipeline.{hpp,cpp}`，`examples/05_reencode/`。
- 新 docs/PAIN_POINTS.md（本轮起的 running 架构痛点日志；见文件顶部说明）。
