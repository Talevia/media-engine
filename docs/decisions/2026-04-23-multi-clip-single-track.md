## 2026-04-23 — multi-clip-single-track (Milestone §M1 · Rubric §5.1)

**Context.** M1 exit criterion「单轨 N clip concat + 裁剪」。之前 loader 强制
`clips.size() == 1`，Exporter 也复制了同一条约束。结果：单个 2s MP4 可以通过 C API
走通，但"把两段 clip 顺序拼起来"这种最基本的剪辑动作就返回 `ME_E_UNSUPPORTED`。前
四轮 M1 已经把 probe / reencode / test / thumbnail 四条 asset-level 管线跑通——多
clip 是 M1 里最后一条 composition-level 能力。

**Decision.**
- **Loader relax** (`src/timeline/timeline_loader.cpp`)：
  - 允许 `clips.size() >= 1`（而非 `== 1`）。
  - `sourceRange.start` 可 `>= 0`（之前强制 0）；`sourceRange.duration` 保持
    `== timeRange.duration`（phase-1 禁速度变换）。
  - **Contiguity invariant**：每个 clip 的 `timeRange.start` 必须等于此前所有 clip
    `timeRange.duration` 的有理数求和。这是 phase-1"no gaps / no overlaps"的硬约束，
    loader 里用不化简的有理加法（`a/b + c/d = (ad+cb)/bd`）。gap 支持等 schema v2。
  - `tl.duration` 现为所有 clip 的 duration 累加（原来单 clip 时等价）。
- **PassthroughMuxOptions refactor** (`src/orchestrator/muxer_state.{hpp,cpp}`)：
  - `PassthroughMuxOptions` 从"单 DemuxContext + 单 URI"换成
    `std::vector<PassthroughSegment>`。每个 segment 带：opened `DemuxContext` + `source_start`
    + `source_duration` + `time_offset`（advisory）。
  - `passthrough_mux(opts, err*)` 不再吃 `io::DemuxContext&` 第一参数；segments 自己
    提供。
  - **Concat timestamp logic**：遵循 libavformat concat demuxer 的"DTS continuity"做法——
    segment 0 原样 pass-through；segment N (N≥1) 为每条输出流计算 `ts_offset` 使其
    第一个包的 DTS 接在 segment N-1 最后一个写入包的 `pts + duration` 上。per-output-stream
    `last_end_out_tb` 数组跨 segment 保持；每 segment 内部用 `ts_offset_set` 延迟初始化
    per-stream offset（视频和音频首包到达时机不一样）。**不用** timeline 的
    `time_offset` 做 PTS 计算（contiguous 保证 concat-demuxer 结果与 `time_offset`
    一致，但后者不稳健：AAC priming 的 -1024 PTS 会导致 seg N 首包 DTS <
    seg N-1 末包 DTS，被 MP4 muxer 拒绝）。
  - **Codecpar compatibility check**：segment 1..N 必须和 segment 0 有**完全一致**的流
    布局（nb_streams + per-stream codec_id / profile / level / width×height / sample_rate
    / ch_layout.nb_channels / extradata）。不一致 → `ME_E_UNSUPPORTED` + 诊断串。这是
    stream-copy concat 的硬要求（output header 按 segment 0 烤出；后续比特流必须匹配）。
  - **Source start / duration**：`source_start > 0` 触发 `avformat_seek_file` 到最近前向
    keyframe。GOP-rounding 是标准"`ffmpeg -ss X -c copy`"行为——精确 cut 需走 re-encode
    path。`source_duration > 0` 用作"stop 点"：读到 `pkt->pts >= src_end` 就结束当前
    segment。
- **Exporter wiring** (`src/orchestrator/exporter.cpp`)：
  - 为每个 clip 构建一个独立的 demux graph（单 node，`IoDemux` kernel），顺序 await
    每个的 `shared_ptr<DemuxContext>`。多 clip 场景下 `graph_cache_` 按 content_hash
    key 去重；同一 URI 的 clip 复用同一 Graph 对象。
  - Worker thread 按 plan 顺序收集 DemuxContexts，构造 `PassthroughSegment` vector，
    单次调用新 `passthrough_mux`。
  - **Reencode path still single-clip**：`reencode-multi-clip` 背后的 encoder-stateful
    跨 clip 逻辑（priming / GOP boundary / bitrate controller reset）是另一层 worm-can，
    放给后续轮。Exporter 在 `reencode && clips.size() != 1` 时返回
    `ME_E_UNSUPPORTED`，错误信息指向 backlog bullet。
  - **Debt bullet**：P2 末尾 append `reencode-multi-clip` 作为顺手记录，未来走 full
    decode → concat buffer → encode 路径。
- **Tests** (`tests/test_timeline_schema.cpp`)：
  - 删原 "phase-1 rejects multi-clip timelines"，换成 "multi-clip single-track with
    contiguous time ranges loads"（验 duration=4s）+ "phase-1 rejects non-contiguous
    clips (gap/overlap)"（第二 clip timeRange.start=0 应被 contiguity 检查拒）。
- **End-to-end smoke**：`01_passthrough` 跑一个构造的双 clip timeline（两份相同的
  `/tmp/input.mp4`，contiguous 2s + 2s），产 4s H264+AAC MP4，ffprobe 0 error。

**Alternatives considered.**
- **用 timeline 的 `time_offset` 做 PTS**（最早的实现）：公式对，但忽略了 AAC priming
  的 -1024 PTS。Seg 2 首 audio 包 PTS = rescale(-1024) + 88200 = 87176，小于 seg 1
  最后 audio DTS 88064 → MP4 muxer "non-monotonic DTS"。修补方式是 per-stream
  `max(raw_offset, last_end)`，但这等同于"DTS continuity"做法的子集，不如直接采用
  FFmpeg 自家习惯。拒。
- **FFmpeg concat demuxer (`-f concat`)**：直接在输入侧做 concat，引擎只开一个
  AVFormatContext。简单，但要求 input file list 提前写到临时文件，不适合流式 /
  动态 timeline。拒。
- **Decode + re-encode 所有 multi-clip**：最通用，但 passthrough 的卖点就是零损失 +
  快，每次都 decode/encode 会丢 10-100x 性能 + 若目标格式是 h264/aac 还会二次损失。
  拒。
- **Hetero-codec concat via bitstream filter chain**：ffmpeg 支持 `h264_mp4toannexb`
  + re-packaging 做不同 extradata 的 concat，但复杂度显著高（BSF 图 + 编码配置对齐
  + 可能的 SPS/PPS rewriting），而且 phase-1 的 VISION 没承诺 hetero-codec 支持。
  拒——同源 codec concat 先落地，hetero 留给未来的专题 bullet。
- **让 Exporter 自己走 libavformat（跳过 graph-demux）**：代码更直接，但会让
  `IoDemux` kernel 在 M1 里变得几乎无用户（仅 example 02_graph_smoke 演示用）。
  保留 graph path 让 M4-M6 的 decode/filter kernel 有可踩的前车。拒。
- **保留 `time_offset` 进 PTS 计算，只在它与 DTS continuity 冲突时 clamp**：中间方案，
  但引入"何时信 timeline、何时信 DTS 连续"的模糊边界。phase-1 contiguous 保证两者
  重合；schema v2 真有 gaps 时会需要统一一个语义。现在不做。拒。

**Coverage.**
- `cmake --build build` + `cmake --build build-rel -DCMAKE_BUILD_TYPE=Release
  -DME_WERROR=ON -DME_BUILD_TESTS=ON` 均 clean。
- `ctest --test-dir build` 3/3 passed (Debug + Release)，新增 2 条 test cases：
  - "multi-clip single-track with contiguous time ranges loads" → 验 duration=4s。
  - "phase-1 rejects non-contiguous clips (gap/overlap)"。
- `01_passthrough` 对原 sample（single clip）仍产 2s H264+AAC MP4，container
  `mov,mp4,m4a,...`, ffprobe 0 error。
- `01_passthrough /tmp/multi_clip.timeline.json /tmp/multi_out.mp4` 对 2×2s 同源 clip
  产 4.02s H264+AAC MP4（duration 上的 0.02s 差是 AAC priming 的标准 tail），ffprobe
  0 error。
- `05_reencode` 对原 sample 仍产 2.02s H264+AAC MP4（reencode 单 clip 路径未动）。

**License impact.** 无新依赖。所有改动落在已有 `FFMPEG::avformat/avcodec/avutil` 链图内。

**Registration.** 本轮动的注册点：
- `TaskKindId` / kernel registry: 未动（仍然是 `IoDemux` 一条，per-clip 复用）。
- `CodecPool` / `FramePool` / resource factory: 未动。
- Orchestrator factory: **Exporter** 内部 worker 改为处理 `std::vector<ClipPlan>` 而非
  单 `(g, term)` 对；外部接口 `export_to` 签名不变。
- 新导出的 C API 函数: 未新增；`me_render_start` / `me_timeline_load_json` 的 ABI 不
  变——loader 只是接受更多合法 JSON，原单 clip JSON 仍解析成功。
- CMake target / install export / FetchContent_Declare: 未动。
- JSON schema 新字段 / 新 effect kind / 新 codec 名: schema 字段未动；loader 接受域
  扩大（clips ≥ 1、sourceRange.start ≥ 0）。向后兼容：原单 clip JSON 仍合法。
- 新 BACKLOG bullet: `reencode-multi-clip`（P2 末尾，顺手记 debt）。
- `docs/PAIN_POINTS.md` 新增 2 条（`av_interleaved_write_frame` 后字段被清空、
  `time_offset` 字段目前不参与 PTS 计算）。
