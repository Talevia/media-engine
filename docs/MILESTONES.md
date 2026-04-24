# Milestones

当前里程碑（本文件唯一的真理源）：

> **Current: M5 — Text + subtitles**

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
- [ ] libass 字幕 track
- [ ] CJK + emoji + 字体 fallback 正确

## M6 — Frame cache + frame server

### Exit criteria
- [ ] `me_render_frame` 返回真实帧
- [ ] disk cache（`cache_dir` 配置生效）
- [ ] `me_cache_stats` / `me_cache_invalidate_asset` 行为与 VISION §3.3 一致
- [ ] Scrubbing 场景下同一时刻重复取帧命中缓存

## M7 — Host bindings

### Exit criteria
- [ ] Kotlin/Native cinterop `.def` + 示例 Gradle 项目
- [ ] JVM JNI 样例（macOS / Linux）
- [ ] 在 talevia 内建一个 `platform-impls/video-media-engine-jvm` 最小 wrapper，跑通 passthrough 替代 shell-out FFmpeg

## M8+ — 待定

HDR（PQ/HLG）、OpenFX host、Android/iOS 打包与 HW 编码路径、网络源（http streaming）、OCIO v2 升级等——等前置里程碑落地再展开。
