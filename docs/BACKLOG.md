# Backlog

`iterate-gap` skill 的唯一任务源。档内按出现顺序挑，不重排。

**格式**：`- **<slug>** — <Gap：现状 / 痛点>。**方向：** <期望动的东西>。Milestone §M<x>，Rubric §5.<y>。`

`<slug>` 是 kebab-case、一眼看出改什么。`debt-*` 前缀表示技术债。

**本文件只由 iterate-gap skill 写**：新任务通过 repopulate 产出；每轮处理完的 bullet 在 `docs(decisions)` commit 里删掉。手工修改请走 `docs(backlog):` 独立 commit 并说明理由。

---

## P0（必做，阻塞当前 milestone）

## P1（强烈建议，M1 收尾或 M2 起步）

- **debt-stub-inventory** — 代码里 stub 散落（cache_*、thumbnail_*、render_frame），没有单一视图看「还有多少 API 没实装」。**方向：** `tools/check_stubs.sh`（grep `ME_E_UNSUPPORTED` 外加白名单），输出未实装函数表。CI / iterate-gap 的 M1 进度可直接读它。Milestone §M1，Rubric §5.2。
- **debt-thread-local-last-error** — `engine_impl.hpp` 目前用 mutex 守 last_error，但 API.md 承诺「thread-local per engine」。**方向：** 换成 `thread_local std::string` slot per engine（用 `std::unordered_map<std::thread::id, string>` 或真正 `thread_local` 变量带 engine 区分），mutex 保留给初始化/销毁。Milestone §M1，Rubric §5.2。
- **debt-update-architecture-md** — `docs/ARCHITECTURE.md` 已加了五模块条目但信息密度偏低。**方向：** 把 "Current implementation state" 表按 graph / task / scheduler / resource / orchestrator 五模块归位重排（目前是"按 C API 函数"组织）；把 Module layout 的五个新目录的 "scaffolded" 状态更到 impl landing 进度。Milestone §M1，Rubric §5.2。

## P2（未来，当前 milestone 不挤占）

- **ocio-integration** — 暂无色彩管理。**方向：** OpenColorIO FetchContent，assets 的 colorSpace → 工作空间转换 → 输出空间。依赖 probe-impl 先落。Milestone §M2，Rubric §5.1。
- **multi-track-video-compose** — 只支持单轨。**方向：** 多 video track 叠加，alpha + blend mode（normal/multiply/screen）。Milestone §M2，Rubric §5.1。
- **audio-mix-two-track** — 音频不合成。**方向：** 2+ audio track 重采样到公共输出率后相加，简单 peak limiter 防爆。Milestone §M2，Rubric §5.1。
- **debt-schema-version-migration-hook** — schema v1 rejection 只认 `== 1`，没有 v2 迁移预演。**方向：** loader 里留 `migrate(v_from, v_to)` 接口，即使只支持 v1 也显式走一遍 migration path，未来 v2 接入零改动。Milestone §M2，Rubric §5.1。
- **reencode-multi-clip** — 当前 reencode path 仍只支持 single clip（Exporter 在 `clips.size() != 1` 时返 ME_E_UNSUPPORTED）；passthrough 已支持 concat。**方向：** encoder state 跨 clip 处理：decoder/filter/encoder 可否复用（同 codec 可复用 encoder ctx + 重置 GOP bookkeeping；不同 codec 必须新开 encoder）；first-frame 应该是 keyframe；AAC encoder 的 priming 在 clip 边界要处理。Milestone §M1 / Rubric 外·顺手记录。
