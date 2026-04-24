# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在当轮的 `feat(...)` commit 里删掉（决策理由写进 commit body，详见 `.claude/skills/iterate-gap/SKILL.md` §7）。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

**Repopulate 纪律（2026-04-23 后）**：每条 bullet 的 `Gap` 部分必须引用 `grep` 查到的具体 `path:line` 证据，不凭印象写"静默丢掉"/"没覆盖"等转述（连续 3 cycle 踩这坑；现在 SKILL.md §R 硬性要求）。

---

## P0（必做，阻塞当前 milestone）


## P1（强烈建议，M6 主线 / 跨 milestone debt）

- **cache-stats-invalidate-impl** — M6 exit criterion "`me_cache_stats` / `me_cache_invalidate_asset` 行为与 VISION §3.3 一致"。`grep -rn 'me_cache_stats\|me_cache_invalidate' src/api` 显示 api/cache.cpp 存在，但 `src/resource/frame_pool.cpp` 的 `hit_count_` / `miss_count_` 是否真推进未验证——frame-server 还没接入，没有客户端。**方向：** 把 frame_pool 加计数器步进（每次 allocate → miss++, 命中已缓存 sample → hit++）；实装 `cache_invalidate_asset` 走 AssetHashCache 清除 content_hash entry + 相关 disk_cache 文件删除。Test: 跑两次相同 frame 请求，第二次 hit_count 应增加；invalidate 后第三次 miss 又回到未缓存。Milestone §M6，Rubric §5.3。
- **scrub-cache-reuse** — M6 exit criterion "Scrubbing 场景下同一时刻重复取帧命中缓存"。`grep -rn 'scrub\|seek.*cache' src tests` 空——没有测 scrubbing 往返行为。**方向：** 依赖 me-render-frame-impl + disk-cache。test: 请求 t=1.0 → t=2.0 → t=1.0 (scrub back)，断言第二次 t=1.0 的请求命中缓存 (hit count +1)，不是 miss。Milestone §M6，Rubric §5.3。
- **async-job-base** — 当前只有 `me_render_start` 一个异步入口，worker→caller 的 error propagation 走 `Job::err_msg` + `me_render_wait` 中转。等第二个异步 API 落地（大概率 M6 frame-server async preview）时抽 `AsyncJobBase` 收口。Milestone §M6，Rubric §5.2。
- **codec-pool-real-pooling** — `src/resource/codec_pool.hpp:6` 注释: "encoder reuse (the "pool" in the name) is deferred"；`codec_pool.cpp` 只 `++live_count_` / `--live_count_`。`reencode-multi-clip` N-segment 开独立 AVCodecContext（`reencode_segment.cpp:112`），但每段 open_decoder 只 ~ms，没 profile 证据表明是瓶颈。**方向：** 等 benchmark 证实瓶颈再加 `get_or_make_decoder(codec_id, codecpar)` + `avcodec_flush_buffers` pool 路径。Milestone §M6-debt (cross)，Rubric §5.3。

## P2（未来，当前 milestone 不挤占）

- **compose-sink-text-clip-wire** — cycle 22 (a10cf70) 落地了 TextRenderer 独立渲染。但 `src/orchestrator/compose_decode_loop.cpp` 的 per-frame video-clip loop 没识别 text clips——有 `Clip::text_params` 的 clip 会被当 video clip 试图 decode → error。M5 Text clip criterion 靠 TextRenderer 单测通过，但端到端 timeline 含 text clip + compose 不 work。**方向：** compose_decode_loop 加 text-clip 分支：检测 `clip.text_params.has_value()` → alloc TextRenderer (canvas W×H) → render onto track_rgba → 跳过 decoder path。同 existing test_compose_sink_e2e 增加 text clip case。Milestone §M5-debt (cross)，Rubric §5.3。
- **compose-sink-subtitle-track-wire** — cycle 20 (79a066d) 落地了 SubtitleRenderer 独立渲染。但 timeline IR 没有 subtitle track concept——TrackKind 只有 video/audio/text。M5 libass criterion 靠 SubtitleRenderer 单测通过，但 timeline JSON 里声明 subtitle track + compose 全流程不 work。**方向：** 扩 TrackKind 加 `Subtitle = 3`；扩 Clip 加 `SubtitleClipParams { std::string file_uri; }` 或 inline text；compose_decode_loop 加 subtitle-clip 分支 (类似 text clip) 调 SubtitleRenderer::render_frame。Milestone §M5-debt (cross)，Rubric §5.3。
- **me-output-spec-typed-codec-enum** — `docs/PAIN_POINTS.md` 2026-04-22 记录：`me_output_spec_t.video_codec` / `audio_codec` 是 `const char*`，每加一个 codec 就要多一个 `is_xxx_spec` helper + 一段 `strcmp` 分支；`video_bitrate_bps` 跨 codec 共享不分。现有两 codec（passthrough, h264）不痛；M3–M4 落第 3、4 个 codec 时是评估"C ABI 引入 typed option union"的决策点。**方向：** 跨 C ABI 的 typed option union 设计（`me_video_codec_t` enum + per-codec `me_h264_opts_t`、`me_aac_opts_t` struct + `me_output_spec_t` 带 tagged pointer）。重大 ABI 演进。Milestone §M6-debt (cross)，Rubric §5.2。
- **vfr-drift-1h-bench** — vfr-av-sync (0d0094b) 的 helper + push_video_frame fix 把 VFR 源 PTS 保留到输出，已 unit-test (`test_reencode_pts_remap.cpp`, 117 assertions)。但"1 ms / 小时"阈值没有端到端 fixture 证据。**方向：** 新 `bench/bench_vfr_av_sync.cpp` — 生成 synthetic VFR input（libav API 直接构造 AVPacket with irregular pts），跑过 reencode，ffprobe output video+audio streams，比较最后帧的 video pts (scaled) vs audio pts (scaled)，assert |diff| / duration < 1e-6 (= 1 ms / 1000s = 1 ms / hour scaled down)。配置 `ME_BUILD_BENCH=ON`. Milestone §M6-debt (cross)，Rubric §5.2。
