## 2026-04-23 — compose-audio-extraction：提取 AudioMixer setup + audio drain 为独立 TU（Milestone §M3 debt · Rubric 外）

**Context.** compose_sink.cpp 在 `debt-split-compose-sink-cpp` cycle (4cd106f) 后降到 609，随后 `audio-mix-sink-wire` / `audio-only-timeline-support` / `transform-animated-integration` 等加了总共 ~25 行回到 633。当前 debt scan 报 P1 长文件（400-700 区间）但未触发 P0 强制 700 阈值。本 cycle **顺手**做一个 decomposition —— 把 AudioMixer setup (~45 行) + audio drain/flush (~50 行) 两段自成逻辑块提到独立 TU `compose_audio.{hpp,cpp}`。ComposeSink::process() 专注于 video compose loop。

Before-state：
- `wc -l src/orchestrator/compose_sink.cpp` = 633。
- compose_sink.cpp line 160-202：AudioMixer setup 块（AudioMixerConfig 构造 + build_audio_mixer_for_timeline 调用 + 错误分流）。
- compose_sink.cpp line 485-534：Audio path 块（mixer drain OR legacy FIFO drain + common flush）。
- 两块都不 touch video state；纯 audio side effects。

**Decision.**

1. **`src/orchestrator/compose_audio.{hpp,cpp}`** 新 TU，两 free function：
   - `setup_compose_audio_mixer(tl, demuxes, shared, pool, out_mixer, err)`：检测 audio tracks → 构造 AudioMixerConfig（从 `shared.aenc` 的 sample_rate/fmt/ch_layout/frame_size 拷贝）→ 调 `build_audio_mixer_for_timeline`。`out_mixer` 空表示 "无 audio tracks / 无 audio 编码器 / 无 pool / loader 宣称 audio 但无 clips" 四种 fall-through 情况。非-NOT_FOUND 错误透传。
   - `drain_compose_audio(mixer, shared, err)`：mixer 非 null → pull → encode → mux 循环；mixer null → legacy `drain_audio_fifo`。两支共用 `encode_audio_frame(nullptr)` encoder flush。
   - 两 helper 都只需 `SharedEncState` 引用 + 一些 timeline metadata，不依赖 compose_sink 的任何私有状态（`track_rgba` / `clip_decoders` / etc）。

2. **`src/orchestrator/compose_sink.cpp`**：
   - 移除 ~45 + ~50 行的两个 inline 块。
   - 替换为 `setup_compose_audio_mixer(tl_, demuxes, shared, pool_, mixer, err)` 和 `drain_compose_audio(mixer.get(), shared, err)` 调用 + 错误分流。
   - `#include "orchestrator/compose_audio.hpp"`。
   - 净效果：633 → **558** 行（-75，-12%）。远离 700 P0 阈值 + 400-700 P1 中段下移到 400-559。

3. **`src/CMakeLists.txt`** + `orchestrator/compose_audio.cpp`。

4. **测试**：不增不减。**行为字节等价** —— 两 helper 逻辑从 compose_sink 搬进来未作修改；所有既有 compose_sink_e2e / audio_only_sink / test_determinism 等依赖通过。31/31 ctest 绿。

**Alternatives considered.**

1. **压力到 < 400 行继续 decomposition** —— 拒：ComposeSink::process() 的 frame loop 本体（~300 行）是高 cohesion 的 video 管线；强拆让理解成本上升，违反本 refactor 的 "move stuff out that doesn't belong" 初衷。audio 两块外移已是干净切口；video 本体该在同一 TU。
2. **Extract encoder/mux setup to yet another TU** —— 拒：setup_h264_aac_encoder_mux 已是外部 helper（encoder_mux_setup.cpp），compose_sink 只调一次。不重复抽象。
3. **Make `setup_compose_audio_mixer` / `drain_compose_audio` class methods on `AudioMixer`** —— 拒：Mixer 不该知道 SharedEncState 或 detail::encode_audio_frame。helper 层负责 orchestration，Mixer 层负责纯 mix math。
4. **Remove SharedEncState dep from helper signatures** —— 拒：两 helper 都需要 aenc / ofmt / out_aidx / next_audio_pts，这些组合就是 SharedEncState 的职责。拆分字段反而增加 signature 噪声。

**Scope 边界.** 本 cycle **交付**：
- 两 audio-side helpers 到独立 TU。
- compose_sink.cpp 633 → 558。
- 零 behavior change。

本 cycle **不做**：
- 新测试（behavior 不变，既有测试作回归覆盖）。
- 删除或改动任何 bullet（这是 pure debt decomposition；`debt-split-compose-sink-cpp` bullet 在前 cycle 已闭环）。

**Coverage.**

- `cmake --build build -j 4` 全绿，`-Werror` clean。
- `ctest --test-dir build` 31/31 suite 绿。
- `wc -l src/orchestrator/compose_sink.cpp` = 558 (was 633)。`wc -l src/orchestrator/compose_audio.cpp` = 93。

**License impact.** 无。

**Registration.**
- `src/orchestrator/compose_audio.hpp/cpp`：新文件。
- `src/orchestrator/compose_sink.cpp`：-75 行 + 2 helper 调用。
- `src/CMakeLists.txt`：+ `orchestrator/compose_audio.cpp`。
- `docs/BACKLOG.md`：**无变化**（不 close 任何 bullet；顺手 refactor 不在 §6 "顺手记 debt" 范畴，但没新增 bullet 也没 close 旧——纯代码质量改进）。

**§M 自动化影响.** M3 current milestone，debt decomposition 不解锁 criterion。§M.1 不 tick。
