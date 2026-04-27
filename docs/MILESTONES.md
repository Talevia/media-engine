# Milestones

当前里程碑（本文件唯一的真理源）：

> **Current: M10 — HDR (PQ/HLG)**

`iterate-gap` skill 在 repopulate backlog 时按「当前 milestone」偏置挑选候选——当前 milestone 未达到 exit criteria 前，跨 milestone 的 gap 进 P2 或被放回"等合适时机"，不抢当前任务资源。

推进 milestone 的条件：**全部 exit criteria 打勾**后，修改本文件顶部的 "Current: " 指针指向下一个 milestone，同 commit 里把新 milestone 的起步任务 seed 进 BACKLOG。milestone 推进决策理由写在 `docs(milestone):` commit 的 message body。

---

## M1 — API surface wired

**目标**：公共 C API 的 7 个头文件每个都有至少一条非 stub 的能跑路径。不追求多轨、不追求效果、不追求色彩管理——把 ABI 跑通、把骨架立稳。

### Exit criteria

- [x] `me_engine_create` / `me_engine_destroy` 可用
- [x] `me_timeline_load_json` 解析 schema v1（phase-1 子集：单轨单 clip）
- [x] `me_render_start` passthrough 路径正确（已验证 2s 测试视频产出合法 MP4）
- [x] `me_probe` 实装：container / codec / duration / W×H / 帧率 / sample_rate / channels 全部从 libavformat 拉
- [x] `me_thumbnail_png` 实装：任意 asset 指定时间点产 PNG
- [x] `me_render_start` 新增至少 1 条 re-encode 路径（建议 h264 via VideoToolbox，mac 上可 HW 加速且 LGPL-clean）
- [x] `me_timeline_load_json` 支持单轨 N clip（concat / trim 组合），phase-1 的"必须单 clip"限制解除
- [x] 单元测试框架接入（doctest），至少 1 条通过的 passthrough 确定性回归
- [x] `me_cache_stats` 返回真实计数（hit/miss/entry_count 不全为 0，配合至少一层 asset 级缓存）
- [x] graph / task / scheduler / resource / orchestrator 五模块骨架就位（见 `docs/ARCHITECTURE_GRAPH.md`），timeline 按段切分，passthrough 已迁到 Timeline → Exporter 执行路径

## M2 — Multi-track CPU compose + color management

**目标**：多轨视频合成、音频混音、显式色彩管理。全 CPU，不碰 GPU。

### Exit criteria
- [x] 2+ video tracks 叠加，alpha / blend mode 正确
- [x] 2+ audio tracks 混音，带 peak limiter
- [x] OpenColorIO 集成，源 / 工作 / 输出空间显式转换，支持 bt709/sRGB/linear
- [x] `Transform`（静态）端到端生效（translate/scale/rotate/opacity）
- [x] Cross-dissolve transition
- [x] 确定性回归测试：同一 timeline 跑两次产物 byte-identical（软件路径）

## M3 — Animated params + GPU backend (Metal first)

**目标**：bgfx 接入、EffectChain shader 合并、关键帧动画。

### Exit criteria
- [x] 所有 animated property 类型的插值正确（linear / bezier / hold / stepped）
- [x] bgfx 集成，macOS Metal 后端可渲染
- [x] EffectChain 能把连续 ≥ 2 个像素级 effect 合并成单 pass
- [x] ≥ 3 个 GPU effect（blur / color-correct / LUT）
- [x] 1080p@60 可实时渲染带 3-5 个 GPU effect 的 timeline

## M4 — Audio polish + A/V sync

### Exit criteria
- [x] SoundTouch 集成，支持变速不变调
- [x] VFR 输入 + 分数帧率输出下 A/V 漂移 < 1 ms / 小时
- [x] Audio effect chain（gain / pan / 基础 EQ）

## M5 — Text + subtitles

### Exit criteria
- [x] Skia 集成
- [x] Text clip（静态 + 动画字号 / 颜色 / 位置）
- [x] libass 字幕 track
- [x] CJK + emoji + 字体 fallback 正确

## M6 — Frame cache + frame server

### Exit criteria
- [x] `me_render_frame` 返回真实帧
- [x] disk cache（`cache_dir` 配置生效）
- [x] `me_cache_stats` / `me_cache_invalidate_asset` 行为与 VISION §3.3 一致
- [x] Scrubbing 场景下同一时刻重复取帧命中缓存

## M7 — Host bindings

### Exit criteria
- [x] Kotlin/Native cinterop `.def` + 示例 Gradle 项目
- [x] JVM JNI 样例（macOS / Linux）

注：本 repo 端的 host binding 表面（JNI + cinterop + 示例 Gradle）已就位；talevia 侧的 `platform-impls/video-media-engine-jvm` wrapper 是跨 repo 集成任务，不计入本 repo M7 exit criteria，作为 BACKLOG `talevia-jvm-wrapper` 跨 repo 跟踪。

## M8 — Playback session（preview player + A/V sync）

**目标**：在已有的单帧 frame-server (`me_render_frame`) 之上新增 timeline 预览播放——引擎内部持有时钟、A/V sync、play/pause/seek 状态机，宿主只负责把帧 / 音频 chunk 上屏 / 喂音频设备。`me_render_frame` 单帧路径保持兼容。

### Exit criteria

- [x] `me_player_t` C API：create / destroy / play / pause / seek / set_video_callback / set_audio_callback / report_audio_playhead / current_time / is_playing
- [x] Producer + Pacer 两线程结构跑通，pause / resume / seek 在 100 ms 内响应
- [x] AUDIO master clock：宿主报 audio playhead 后视频帧跟随，drift P99 < ½ frame_period（沿用 M4 对 export 的 < 1 ms / 小时口径）
- [x] WALL master clock：timeline 无音频或 `audio_out.sample_rate == 0` 时回退，play 5 s 漂移 < 1 frame
- [x] seek 不污染 `OutputCache` / `disk_cache`（回扫旧时间命中），`AudioMixer` 销毁重建正确
- [x] `examples/10_player_pause_seek` 演示主线程 1 s 后 pause / 1 s resume / seek t=2 s / pause 全流程
- [x] 现有 `tests/test_frame_server*` / `examples/08_frame_server_scrub` 不 regression
- [x] `docs/ARCHITECTURE_GRAPH.md` §三种执行模型 (c) 已就位（已在 docs(vision) commit 落定）

## M9 — Performance budgets & observability harness

**目标**：把 `VISION.md` §5.7 从「志愿」转成「守护」。每一条判断题都有具体的测量点 + 回归门槛，回归会 fail，预算可在 commit 里看见——不再有"无跟踪 = 预算 ∞"的 silent drift。已落地的部分（4 个 fail-on-budget bench）保留；缺的部分（内存峰值、API 表面、二进制尺寸、缓存命中率下界）补齐到「最小守护」级别——具体阈值后续 iterate-gap 调，但回归门槛此 milestone 必须就位。

### Exit criteria

- [x] §5.7-1 FramePool / OutputCache / disk_cache 命中率在至少一个 bench 程序里被 dump（不只 example）— `bench/bench_thumbnail_png.cpp` 末尾 dump `me_cache_stats`
- [x] §5.7-3 单次渲染峰值 RSS 在至少一个 bench 程序里被 dump，跨平台（macOS mach + Linux getrusage）— `bench/peak_rss.hpp` + `bench_thumbnail_png` pre/post delta
- [x] §5.7-4 `include/media_engine/*.h` 公共 me_-prefix 函数数量 + struct 字段数有显式 budget 文件 + ctest 守护，超出 budget 必须显式 bump 数字（commit 可审计）— `tools/api_surface_budget.txt` + `tests/test_api_surface_budget.cpp`
- [x] §5.7-5 release `libmedia_engine` 静态/动态库尺寸有 budget 文件 + ctest 守护，超出门槛 fail — `tools/lib_size_budget.txt` + `tools/check_lib_size.sh` + ctest `lib_size_budget`
- [x] §5.7-6 同一 timeline 重渲染缓存命中率下界有数值断言（不只 dump）的 test，若命中率塌穿地板会 fail — `tests/test_cache_hit_rate_lower_bound.cpp`
- [x] `VISION.md` §5.7 每条判断题旁边引用对应守护代码（bench / test 路径），让 rubric 从"问句"变"指针"— §5.7 改写成 6 行表格指对应代码

## M10 — HDR (PQ/HLG)

**目标**：BT.2020 + PQ + HLG 端到端，10 / 12-bit 编解码全链，HDR metadata 不丢失，SDR ↔ HDR 互转 deterministic。配合 §3.4 LGPL 红线（HEVC 软件 fallback 不得引入 GPL libx265），HW HEVC 编码（VideoToolbox）是 ship path、SW 是受限 fallback。

### Exit criteria

- [ ] timeline schema 支持 `colorSpace.{primaries=bt2020, transfer=smpte2084|arib-std-b67, matrix=bt2020nc, range=full|limited}`，schema validation 拒绝非法组合
- [ ] decode 路径覆盖 HEVC Main 10、VP9 Profile 2 (10/12-bit)、AV1 10-bit；每路径有像素级 round-trip 测试
- [ ] encode 路径：HEVC Main 10 via VideoToolbox（HW HDR），软件 fallback 走 LGPL-clean 编码器（SVT-HEVC Apache 2.0 候选；libx265 GPL 排除）；SW 路径标记非确定性 + 受限输出（1080p 上限 / 失败时显式错）
- [ ] `me_probe` 抽取 HDR metadata：MaxCLL / MaxFALL / MasterDisplay primaries + luminance（合并 BACKLOG `me-probe-hdr-metadata`）
- [ ] OCIO 升级：内置 + 可注入 PQ / HLG / ACES config（合并 BACKLOG `ocio-config-env-override`）
- [ ] SDR ↔ HDR 互转：tonemap (HDR → SDR via Hable / Reinhard / ACES，显式 effect kind)、inverse-tonemap stub（标记非确定性，仅 HDR 输出场景使用）
- [ ] bench: `bench_hdr_roundtrip` HDR → HDR 透传位精度 + HDR → SDR tonemap 漂移预算
- [ ] tests: `test_hdr_metadata_propagate`（probe → timeline → encode → probe 链上不丢字段）、`test_pq_hlg_roundtrip`（解 → 渲 → 编后像素与原始误差 < ε）
- [ ] §5.7-5 lib_size_budget 跟随实际链入的 codec 路径调整（commit body 解释）

## M11 — ML 推理基建 + 第一波 detection-driven effects

**目标**：建立 §3.5 ML 推理基建——推理 runtime 可选模块、推理结果作为 typed asset、contentHash 缓存、模型 lazy load + license 校验、CPU FP32 reference path——并在此之上交付第一波 ML-driven effect（人脸 + 人体）。先把基建建对，再加 effect；不建基建直接 patch effect 会让后续每个 ML 任务重复造轮子。

### Exit criteria

- [ ] CMake `ME_WITH_INFERENCE=ON / OFF` 选项；OFF 时 `lib_size_budget` 不变（核心 link graph 不污染），ON 时进入独立 budget 行
- [ ] 推理 runtime 接入：CoreML（macOS / iOS）+ ONNX runtime（跨平台，CPU FP32 reference）；TFLite 暂不接（按需后续）；CUDA / MKL / 闭源 SDK 一律不链
- [ ] 推理 asset 的 timeline schema 类型：`landmark` (Nx2 floats / frame, confidence)、`mask` (alpha sequence)、`keypoints` (skeleton with connectivity)；每类带 `model_id` + `model_version` + `quantization` 必填字段
- [ ] 推理 asset 走 §3.3 contentHash 缓存：key = `(model_id, model_version, quantization, 输入帧 hash)`；同输入跨调用复用，回归测试覆盖
- [ ] 模型权重 lazy load：host 实现 `me_model_fetcher_t` callback；engine 校验 content_hash + license 白名单（Apache / MIT / BSD / CC-BY），non-commercial / GPL / unknown license 拒载
- [ ] 至少 2 个 ship-path model 跑通：face landmark（候选 BlazeFace + face_landmarks_v2，TFLite Apache）+ portrait segmentation（候选 SelfieSegmentation，Apache）；CPU FP32 reference 与 CoreML / ONNX runtime HW path 误差 < ε
- [ ] effect kind 扩展可声明 ML-asset 输入：参数 typed schema 增加 `landmark_asset_ref` / `mask_asset_ref` / `keypoint_asset_ref` 等引用类型
- [ ] 第一波 detection-driven effect（≥ 3 个）：
    - `face_sticker` — 贴纸按 landmark 平移 / 缩放 / 旋转吸附面部
    - `face_mosaic` — 跟随 landmark bbox 的局部模糊 / 像素化
    - `body_alpha_key` — portrait segmentation mask 作前景 alpha
- [ ] 每个 effect 有像素级回归测试 + 至少一个 example（候选：`12_face_sticker`、`13_body_alpha_key`）
- [ ] §5.7-5 `lib_size_budget` 加 `Release_with_inference` 行，独立预算覆盖推理 runtime + reference 模型（host 注入的 ship 模型不计入）
- [ ] §5.8 自查 rubric 在 PR template 或 iterate-gap skill 提示中引用，挡住越界 ML 任务

## M12 — Effect 库扩展（确定性 GPU shader 类，不依赖 ML）

**目标**：补齐"传统创意特效"覆盖面——颜色 / 风格化 / 模糊变体 / 几何变形——全部 deterministic GPU shader（§3.1 软件路径 bit-identical），不引入新依赖。M11 的 ML-driven effect 是"区域感知"维度，M12 是"全画面创意"维度，两条互补。

### Exit criteria

- [ ] 颜色：tone curve、hue / saturation、vignette、film grain（带 deterministic seed 入参）
- [ ] 风格化：glitch、scan lines、chromatic aberration、posterize、ordered dither
- [ ] 模糊变体：motion blur（按 transform 速度 derive，或参数指定向量）、radial blur、tilt-shift
- [ ] 几何：warp（control points + animated property）、displacement (texture-driven)
- [ ] 全部进入 EffectChain 合并路径（M3 §EffectChain 骨架已就位），≥ 3 effect 单 pass 案例
- [ ] 每个 effect kind 至少 1 个像素级回归测试 + 1 个 timeline JSON 示例
- [ ] 不新增第三方依赖（CMakeLists 不动 FetchContent / find_package）
- [ ] §5.7-5 lib_size_budget 在 effect 数量翻倍后保持在预算内（不行就 commit body 显式 bump，但优先用 shader uber-shader 复用挤空间）

## M13+ — 待定

OpenFX host、Android / iOS 打包与 HW 编码路径、网络源（http streaming）、第二波 ML 任务（OCR / scene understanding / speech recognition / activity detection）等——等前置里程碑落地再展开。
