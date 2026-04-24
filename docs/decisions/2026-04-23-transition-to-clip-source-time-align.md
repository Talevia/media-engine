## 2026-04-23 — transition-to-clip-source-time-align：cross-dissolve 结束后 to_clip 解码器帧精确回跳对齐 schema (Milestone §M2 · Rubric §5.1)

**Context.** `cross-dissolve-transition-render-wire` 上线时明确记录了一个 phase-1 限制（decision doc 2026-04-23 / code comment `compose_transition_step.hpp:31`）：

> Transition 窗口内 to_clip decoder 顺序 pull，window_end 时解码器已消耗 `duration × fps` 帧（跨整个 window），接入 single-clip 区间后播放内容相对 schema 领先 `duration/2`。fixture 是慢变化 gradient 时不可见，真实 content 会产生不一致。

Before-state grep evidence：
- `src/orchestrator/compose_transition_step.cpp:113-128`（pre-cycle）：`compose_transition_step` 按 `duration × fps` 个 output frame 顺序 pull `td_to`，无任何对齐机制。
- `src/orchestrator/compose_sink.cpp:294-315`（pre-cycle）：SingleClip branch 无差别调 `pull_next_video_frame`——不知道该 clip 是否刚从 transition 出来。
- `grep -n 'avformat_seek_file\|avcodec_flush_buffers' src/orchestrator/` 返回空——compose 路径内没有 seek 能力。
- 已有 frame-accurate seek 先例在 `src/api/thumbnail.cpp:57-118 / 179-213` + `src/orchestrator/reencode_segment.cpp:173` + `src/orchestrator/muxer_state.cpp:188`，可借鉴 pattern（`avformat_seek_file BACKWARD` → `avcodec_flush_buffers` → decode 丢帧到 target）。

本 cycle 把这个已知限制清掉。

**Decision.** 四处改动 + 一处公共 helper + 测试：

1. **TrackDecoderState 新增布尔旗标** (`src/orchestrator/frame_puller.hpp`)。`bool used_as_to_in_transition = false`，语义："最近一次被 compose_transition_step 当做 to_clip 拉过"。

2. **新公共 helper** `seek_track_decoder_frame_accurate_to(TrackDecoderState&, me_rational_t target_source_time, std::string* err)` (`frame_puller.{hpp,cpp}`)。三步 pattern 照搬 thumbnail.cpp：
   - `avformat_seek_file(fmt, -1, INT64_MIN, target_us, target_us, AVSEEK_FLAG_BACKWARD)` —— 落到 target 之前的最近关键帧。
   - `avcodec_flush_buffers(dec)` —— 清解码器状态。
   - 内循环 `avcodec_receive_frame` + `av_read_frame` + `avcodec_send_packet`，把 `pts < target_pts_stb` 的帧 `av_frame_unref` 丢弃；第一个 `pts >= target` 的帧留在 `td.frame_scratch`。
   返回约定：ME_OK 时 `frame_scratch` 持有 target 帧，caller 当做 `pull_next_video_frame` 刚返回来处理（`frame_to_rgba8` + `av_frame_unref`）。ME_E_NOT_FOUND 表示解码器在到达 target 前 drain 了（asset 比 target 短）。ME_E_IO / ME_E_DECODE 按错误来源分类。

3. **compose_transition_step 设置旗标** (`compose_transition_step.cpp:128-135`)。`to_pull` 成功返回后 `td_to.used_as_to_in_transition = true`。from_pull 无需设（from_clip 在 window 结束时不再参与，不需要回跳对齐）。

4. **compose_sink SingleClip branch 消费旗标** (`compose_sink.cpp:303-322`)。如果 `td.used_as_to_in_transition` 为 true：
   - 先清旗标（避免后续 SingleClip pull 再次触发）。
   - 调 `seek_track_decoder_frame_accurate_to(td, ta.source_time, err)`——`ta.source_time` 是 `frame_source_at` 为当前 T 算出的 schema-aligned `source_start + (T - time_start)`。
   - 分支 pull_s 返回值的后续处理（NOT_FOUND → continue、其它 ok / err 处理）保持不变。
   否则走原有的 `pull_next_video_frame` 路径。后续的 `frame_to_rgba8` + `av_frame_unref` 对两条路径结果一致。

5. **Tests** (`tests/test_frame_puller.cpp`)：
   - `seek_track_decoder_frame_accurate_to: lands on target frame after decoder advanced past it` —— 开 fixture + decoder，pull 15 帧 advance 过 target，`seek_track_decoder_frame_accurate_to(target={10,25})` 后 assert `frame_scratch->pts ∈ [10, 11]`（MPEG-4 Part 2 无 B-frame 时 pts 逐帧递增，对齐成功 iff 落到 frame 10）。这是本轮的核心 tripwire——证明 helper 真能"倒退 + 前进到精确帧"。
   - `seek_track_decoder_frame_accurate_to: unopened td returns ME_E_INVALID_ARG` —— null demux / null dec 防御。
   - `seek_track_decoder_frame_accurate_to: negative target clamps to zero` —— 负 `target_source_time` 不崩，clamp 到 0 并返回第一帧。

**Alternatives considered.**

1. **Preroll 模式**——在 window_start 之前把 to_clip decoder "预先"倒退到 `source_start - D/2`（概念上 negative），让 D 帧自然解码到 window_end 时达到 schema 位置。拒：source_start 通常为 0，负值不可表达；除非 clip 带 "handle"（source 侧的额外预留素材，schema 未建模），这个方案无法落地。真要做需要 schema 新增 handle 字段 + 大范围 IR/loader 改动——远超本 bullet scope。

2. **per-sample 插值 + decoder pull 减频**——transition 窗口里 to_clip 只 pull `D/2 × fps` 帧而不是 `D × fps`（跳过前半窗口的 pull，靠"持住首帧"填充）。拒：需要增加"to_clip 对 window_start 不起作用"的 FSM 状态；cross_dissolve kernel 要接 `is_frozen` 参数；破坏 ComposeSink 当前"每 T 一帧一个 track" 的统一接口。complexity explosion 换来的好处仅是"避免 seek"。

3. **编译时关闭 transition 对齐，保留已知限制不修**——拒：phase-1 限制 comment 明文说 "真实 content 会产生不一致"。cross-dissolve 已经在 M2 exit criterion 里打了勾（evidence 基于慢 gradient fixture），但真实 content 用 cross-dissolve 会出现观感 bug。不修就是让 M2 exit 虚假打勾。

4. **Seek + 不做 frame-accurate drop（仅 seek 到关键帧）**——拒：libav seek 落到 BACKWARD 关键帧（GOP=25 意味着一个 GOP 结束才有下一个 KF，最坏情况 target 落在 GOP 内需要 back-seek 到 GOP start）。没有 drop-to-target 的话，pull 返回的是 KF 那个帧，可能比 target 早 ~1s。对齐无效。drop-to-target 是必要补丁，thumbnail.cpp 先例也这么做。

5. **一次 Monitor-like 检测 decoder drift（记录 pulled frame count vs schema expected）并自适应 seek**——拒：需要 per-decoder 精确簿记 "已 pull 帧数" + per-pull schema 对比。旗标 bit + 单次 seek-on-handoff 轻量得多；drift 检测适合将来真实 VFR 源 / B-frame reorder 场景，本 bullet 只解决已知的 transition window 领先 `D/2`。

6. **在 compose_transition_step 内部直接 seek（不经过旗标）**——拒：compose_transition_step 函数当前职责是"blend 一帧"，加 seek 语义会把 "window end 检测" 推到该函数里，它不持有 "next T 是 SingleClip 吗" 的信息。旗标放在 TrackDecoderState（数据结构自然 per-clip），compose_sink 的 per-T-per-track 循环自然有这个信息。层次清楚。

**Coverage.**

- `tests/test_frame_puller.cpp` 新 3 TEST_CASE 全绿，包括 pts 精确对齐的核心验证。
- `tests/test_compose_sink_e2e.cpp` 既有 "single-track with cross-dissolve transition renders" continue 绿——证明 transition→SingleClip 切换路径无 regression。
- `test_compose_cross_dissolve` kernel test 继续绿（helper 改动不涉及 kernel 数学）。
- `test_compose_active_clips` resolver test 继续绿（resolver 语义未变）。
- 所有 31 个 ctest 测试套件 Debug + Release `-Werror` 全绿；`01_passthrough` 端到端 + ffprobe 验证产出 h264 + aac MP4 不 regress。

**License impact.** 无新依赖。

**Registration.** 无注册点变更：
- C API surface：不变。
- Kernel / codec / factory / scheduler 注册表：不变。
- CMake：不变。
- JSON schema：不变（本 cycle 是运行时行为修复，不涉及 schema）。

**§3a self-check.** 10 条全绿：typed params ✓、rational time（target 是 `me_rational_t`，转 us 只在 libav boundary）✓、公共头不变 ✓、C API 不越界 ✓、无 GPL ✓、确定性（seek + drop 对同一 fixture 同一 target 可复现；seek_file BACKWARD 行为 deterministic）✓、无新 stub ✓、无 GL ✓、schema 兼容 ✓、ABI 不破 ✓。

**§M 自动化影响.** 本改动把 M2 exit criterion "Cross-dissolve transition" 的 evidence 从"能渲 + 慢 gradient fixture 看不出 bug"升级到"真实 content 也能 frame-accurate"。Exit criterion 本已 ticked（上 cycle），本轮不改 §M.1。
