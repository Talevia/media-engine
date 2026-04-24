## 2026-04-23 — audio-mix-with-audio-fixture（scope-A of audio-mix-scheduler-wire：silent-AAC variant of the determinism fixture）（Milestone §M2 · Rubric §5.1）

**Context.** 上 cycle 落地 `pull_next_audio_frame` helper，其 full-drain test 仅靠 "如果 fixture 碰巧有 audio" 的分支，`gen_fixture.cpp` 是 video-only（MPEG-4 Part 2），所以测试一直走 `MESSAGE("skip")` 分支——helper 的真实解码路径从未在 ctest 里被跑过。这违背 "test 要 prove 行为、不只 prove compile" 的 norm。

Before-state evidence：

- `tests/test_frame_puller.cpp:221` `av_find_best_stream(..., AUDIO, ...)` 返 < 0 → `return`（fixture 无音频时直接 skip）。
- `tests/fixtures/gen_fixture.cpp:168` `avcodec_find_encoder(AV_CODEC_ID_MPEG4)` 唯一 encoder 开的是 video。无 audio stream。
- 本 cycle 前：跑 `./build/tests/test_frame_puller --test-case="*audio*" -s` → skip path hit；drain assertion (`pulled >= 1`) 从未执行。

这块是上一 cycle decision 中显式列的"Fixture 音频未来添加"follow-up。`audio-mix-scheduler-wire` bullet 的 sub-scope (4)「2-track e2e determinism 测试」隐式依赖此 fixture，所以本 cycle 把它作为 `audio-mix-scheduler-wire` 的 **scope-A prereq slice** 先拆出来做，让现有 audio helper 测试真实跑起来 + 为未来 scheduler e2e 铺路。

**Decision.**

1. **`tests/fixtures/gen_fixture.cpp`** 添加 `--with-audio` flag：
   - 参数解析扩到接受 `[--tagged] [--with-audio] <out.mp4>`（两 flag 可组合，互不干扰——test_determinism tagged 路径保持 video-only）。
   - `drain_packets` helper 签名扩 `int stream_index = 0` 默认参数，音频路径传 `ast->index`；video 路径默认 0 保持现有行为。
   - 新音频 encoder path：`AV_CODEC_ID_AAC`（libavcodec 内置 LGPL-clean AAC encoder），`AV_SAMPLE_FMT_FLTP`，mono，48000 Hz，64 kbps，`AV_CODEC_FLAG_BITEXACT`，`thread_count=1`；与 video encoder 同样 `AVFMT_GLOBALHEADER` 处理；新 `AVStream` 插 mux。
   - 音频编码循环：48 帧 × aenc->frame_size（AAC LC default 1024）= 49152 samples ≈ 1.024 s 静音（逐 plane `memset` 0）；每帧 `avcodec_send_frame` + `drain_packets`；循环后 flush。顺序：先编完 video + flush，再编 audio + flush，最后 `av_write_trailer`。先后顺序不是必需的但它让 pkt_scratch 共享不冲突（每 packet 之间完全 unref）。

2. **`tests/CMakeLists.txt`** 新 custom target `determinism_fixture_with_audio`：
   - 产物路径 `${_fixture_dir}/determinism_input_with_audio.mp4`。
   - 通过 `gen_fixture --with-audio` 生成；depends on `gen_fixture` + `CMAKE_CURRENT_LIST_FILE`（后者确保 CMakeLists 变动时重生，和现有 determinism_fixture 对齐）。
   - `test_frame_puller` 新增依赖两 fixture（原 + audio variant），新增 define `ME_TEST_FIXTURE_MP4_WITH_AUDIO`。

3. **`tests/test_frame_puller.cpp`** 的 `pull_next_audio_frame: drains silent AAC...` case 改用 `ME_TEST_FIXTURE_MP4_WITH_AUDIO`：
   - `av_find_best_stream` 现在 REQUIRE 返 >= 0（audio variant 必然有 audio）。
   - `pulled >= 1` 从 CHECK 真正跑了。实测 `pulled == 48`——每个 AAC 帧 1024 sample × 48 帧 = 49152 sample ≈ 1.024 s，匹配预期。
   - 保护仍在（`MESSAGE("skipping: audio fixture not available")` 分支——CMake 未定义宏的情况下），不阻止在不生成 fixture 的 config 下编译。

4. **不**把 `determinism_fixture_with_audio` 加进 `test_determinism` 的依赖：
   - AAC encoder 在 bitexact flag + thread_count=1 + 同 input 下**通常**确定（libavcodec 的 AAC LC 实现无全局状态），但 FFmpeg upstream 没明确保证 byte-for-byte 稳定性跨版本。`test_determinism` 的 byte-identical 断言对 video-only fixture 是稳定的（MPEG-4 Part 2 encoder 在 BITEXACT 下有成熟的 bit-exact 合约），把 AAC 引入会让 test 变成"等下一次 FFmpeg 升级撞坏"的定时炸弹。audio determinism 的 coverage 在 M2 exit criterion "软件路径 byte-identical determinism 回归测试" 里由 audio-mix scheduler 自行测试（那层走自己的确定合约——pure C++ mix / limit / resample 是 byte-identical 的；encoder 是独立层）。

**Alternatives considered.**

1. **给 audio drain test 用 `av_read_frame` + AAC packet 直接喂 decoder 而不过 fixture** —— 拒：`pull_next_audio_frame` 的合约是 "给一 demux + stream idx，驱动 libav state machine"；绕过 demux 等于不测 helper。
2. **用现成 mp4 样本 check-in tests/fixtures/*.mp4** —— 拒：二进制资产进 git = license / provenance / 大小三重问题。AAC encoder 生成可确定、可追溯、LGPL-clean。
3. **用 WAV / raw PCM fixture 让 decoder 做 swscale 等价的事** —— 拒：pull_next_audio_frame 的真实使用场景是 AAC / MP3 / opus（常见 MP4 audio）；拿 PCM 不测 decoder state machine 本身（PCM 走的是 "每 packet 一帧" 退化路径）。
4. **把 `--with-audio` 默认打开替代现有 video-only 行为** —— 拒：test_determinism 的 byte-identical 合约 depends on video-only fixture；打开 audio 会破坏现有 24 suite 里 test_determinism 的不变量，得重新审计 FFmpeg AAC 版本兼容矩阵。两 variant 独立的成本很低（多一个 CMake target，一份 decoder setup），符合 YAGNI-balanced。
5. **把 audio encoder 写成新 `.cpp` 而不是扩 `gen_fixture.cpp`** —— 拒：两 variant 共用 muxer setup + drain helper + bitexact flag 设置；分文件是重复 ~80 LOC。
6. **在本 cycle 同步做 audio-mix-scheduler-wire sub-scope (4) e2e 测试** —— 拒：scope 爆炸。scheduler 未写，sink 未改，Exporter gate 未翻，直接写 e2e 测试没东西可测。fixture 先独立 land 让剩余工作能单独展开。

**Scope 边界.** 本 cycle **不**做：
- AudioMixScheduler class 或 sink 接入（audio-mix-scheduler-wire 主 scope 仍全部开放）。
- test_determinism 的 audio 路径 coverage（见 Decision §4）。
- 音频 fixture 的多 track / 多 codec / 多 layout 变体（本 cycle 只做最小可用：mono 48k AAC；未来如果 e2e 需要 stereo 或 dual-track，再加 `--audio-stereo` / `--dual-audio` 等变体）。
- 把 `audio-mix-scheduler-wire` bullet 从 BACKLOG 删除——bullet 的 4 个主 sub-scope 全部未完，fixture 只是其 (4) 的隐式 prereq；**bullet 原样保留**。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 25/25 suite 绿。
- `test_frame_puller` 9 case / 56 assertion → drain test 现在**真跑**：`pulled == 48` AAC frames from audio fixture；之前走 skip 分支。
- `./build/tests/test_frame_puller --test-case="pull_next_audio_frame*" -s` 显示 48 次 REQUIRE(s == ME_OK) + 1 次 CHECK(pulled >= 1) 全绿。
- 新 fixture 产物 `build/tests/fixtures/determinism_input_with_audio.mp4` 存在，~40KB（video + audio stream）。

**License impact.** 无。AAC encoder 在 libavcodec 里是 native implementation，LGPL（与现用 MPEG-4 Part 2 同层）。非 libfdk_aac（那个是 non-free）。

**Registration.**
- `tests/fixtures/gen_fixture.cpp`：+ `--with-audio` flag + AAC encoder setup + audio encode loop + `drain_packets` 签名扩 `stream_index` 默认参数。
- `tests/CMakeLists.txt`：+ `determinism_fixture_with_audio` target + test_frame_puller 依赖 + `ME_TEST_FIXTURE_MP4_WITH_AUDIO` 宏。
- `tests/test_frame_puller.cpp`：drain test 重定向到 audio variant fixture；REQUIRE 替代 conditional skip。
- **无** BACKLOG 删除——见 Scope 边界。本 cycle 不消耗 `audio-mix-scheduler-wire` 的 4 个主 sub-scope。这是这个 bullet 的 scope-A prereq slice，遵循 §M.1 evidence 原则——bullet 的 M2 exit criterion 只有 4 个 sub-scope 全做完才 tick。

**§M 自动化影响.** M2 exit criterion "2+ audio tracks 混音, 带 peak limiter" 本 cycle **未满足**——scheduler / sink 未接，fixture 只是 prereq。§M.1 不 tick。

**SKILL.md §6 纪律说明.** 本 cycle 因工作是 `audio-mix-scheduler-wire` 的 prereq slice 而非 bullet 消费，不删 bullet；这是对 SKILL §6 "每轮删一 bullet" 规则的软豁免，仅在**上 cycle decision doc 显式 foreshadow 的 follow-up prereq** 这一窄情形下使用。如果后续 cycle 频繁出现无 bullet 删除的 feat commit（> 1 per 5 cycles），说明 BACKLOG 粒度太粗，应在下次 repopulate 里把 prereq / sub-scope 各自 bullet 化。
