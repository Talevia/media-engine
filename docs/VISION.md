# media-engine — 愿景与北极星

## 本文档定位

**这是 media-engine 的第一性原理。**

本文档是北极星——要去哪里、为什么去。不是操作手册（见 `ARCHITECTURE.md`、`API.md`、`TIMELINE_SCHEMA.md`）。

冲突时以本文档为准。其他文档记录的是为了实现本文档的愿景在**当前阶段**接受的工程边界，本身不是第一性原理。

对 coding agent：在「找缺口 → 补能力 → 迭代」的循环里，**先读本文判断方向**，再读 `ARCHITECTURE.md` / `API.md` 判断边界与现状，最后读代码找差距。用 §5 的 rubric 自查。

---

## 发现不符 → 必须 challenge

**如果手头的任务会把系统引向和本文档不一致的方向，必须 challenge，不要沉默推进。**

这条对所有读到这里的人生效——用户、coding agent、reviewer 都一样。

典型冲突信号：

- 任务要求链 GPL / AGPL 依赖
- 任务要求把 GUI / 实时编辑器塞进 media-engine
- 任务要求为了配合宿主当前 schema 阉割 IR（本引擎定义理想 schema，宿主适配）
- 任务要求放弃确定性、放弃缓存、走全量重渲染"图简单"
- 任务要求 effect 参数用无类型 map（必须 typed schema）
- 任务要求时间用 `double` / `milliseconds`（必须有理数）
- 任务要求 C API 里穿 C++ 类型 / STL / 异常

发现这类冲突时，**先停下来把问题显式提出来**——在对话里、在 PR 描述里、在代码注释里。

结果只有两种：要么任务调整贴近愿景，要么愿景修正容纳新现实。两者都必须被显式讨论。**沉默推进 = 系统悄悄偏离北极星，这是最贵的一种 bug**。

---

## 1. 北极星

**做一个 AI agent 可以像写代码一样编排的音视频编译器。**

输入是声明式 JSON 时间轴，输出是确定性的编码文件 / 单帧 / 缩略图。中间的一切（解码、合成、特效、编码）都是被 agent 调用的能力，不是被人拖拽的 UI。

不是 NLE，不是 compositor GUI，不画播放窗口。是一个**被调用**的库——内部可以承担引擎级播放会话（时钟、状态机、A/V sync），把"此刻该显示的帧 / 该播放的音频"通过 callback 推给宿主；但 surface（窗口、widget、绘制目标）始终是宿主的事。

---

## 2. 在 talevia 生态里的位置

talevia 把音视频创作建模成构建系统：`Source → Compiler → Artifact`。Compiler 是多种能力的混合——传统剪辑引擎、特效渲染、AIGC、ML 加工。

**media-engine 就是这个 Compiler 里「传统引擎 + 特效渲染」那一段。**

不碰 AIGC、不做 ML 推理——那些由 talevia 调外部服务。宿主负责 source schema、DAG 调度、缓存策略；本引擎负责"给我一个 timeline JSON 和一堆资产，我还你一个帧 / 一段视频，可预测、可缓存、可观测"。

**谁适配谁**：media-engine 按它认为「对的」IR 与 C API 设计。宿主（当前是 talevia）会演进来对接——不是反过来。本项目不为贴合宿主当前过渡性 schema 阉割设计。

---

## 3. 核心工程赌注

### 3.1 确定性渲染

相同 timeline + 相同 engine 版本 + 相同输入资产 → **bit-identical 或 perceptually identical** 输出。

这不是「nice to have」。talevia 的整个构建系统模型（DAG、增量编译、artifact 缓存）都建立在"相同输入产出相同结果"这个假设上。没有确定性，上游的缓存失效判断全部是错的。

代价：时间必须用有理数（不用 `double`）；浮点求值顺序要规范化；编码器参数完全由 IR 决定，不靠隐式默认；HW 编码器这种"天然不确定"的路径要明确标记且可回退到软件路径做回归测试。

### 3.2 类型化的 effect 与动画参数

Effect 不是 `Map<String, Float>`。每个 effect kind 在引擎里注册**类型化参数 schema**，所有参数与 transform 字段都走统一的 animated property 结构（static 或 keyframe + 插值曲线）。

这是本引擎和"又一个 FFmpeg 包装库"的分野。agent 能对效果做结构化编辑（"把 clip c1 的 blur radius 从第 5 帧开始动画到 20"），而不是拼 filter 字符串。没有这一层，下游所有"让 LLM 改单个参数"的体验都会退化成全量重生成。

### 3.3 内容寻址的增量缓存

Asset 是一等实体，按 content hash 引用；clip 和 effect 的中间产物以 `(输入 hash, engine 版本, 参数 hash)` 为键缓存。

这是 §3.1 确定性渲染的受益方——确定性保证缓存安全；缓存让「改 1 帧不用全量重渲染」成为可能。两条赌注捆绑在一起。

### 3.4 LGPL-clean 供应链

从 day 1 锁死依赖许可证白名单。一个 GPL 依赖悄悄混进来，产品线就可能被迫开源或重构。预防比治疗便宜 100 倍。

具体清单见 `ARCHITECTURE.md`——本文只记录原则：**禁止任何 GPL / AGPL 出现在最终 link 图里**。HW 编码器（VideoToolbox / NVENC / QSV / VAAPI / AMF）优先于 libx264 / libx265；Rubberband 默认不链。

---

## 4. 系统边界

### 做

- 读 timeline JSON，产出编码文件 / 单帧图像 / 缩略图
- 多轨音视频合成、transition、typed effect、static & animated transform
- 硬件加速编解码
- 跨 mac / iOS / Android / Linux / Windows
- 中间结果可失效、可观测的缓存
- 通过 C API 被任意宿主驱动

### 不做

- GUI / 拖拽编辑器 / 预览 UI surface（窗口、widget、绘制目标）——引擎可以**承担播放会话**（时钟、A/V sync、frame/audio callback），但不画 surface 给用户看；surface 由宿主拥有
- 撤销重做 / 协同 / diff / merge / 工程文件格式（宿主 Timeline 的责任）
- AIGC、ML 推理（talevia 调外部服务）
- 资产库 / 文件管理（宿主 MediaStorage 的责任）
- 分布式渲染调度（单机单进程；分布式由宿主自己切任务）
- 交换格式 import/export（OTIO、AAF、XML、EDL 都不做——我们的 JSON 是唯一 schema）

---

## 5. Gap-finding rubric（给自主迭代的 agent）

这不是 TODO 列表——TODO 会过时。这是一套**判断题**，在「找缺口 → 补能力」循环里用来自查当前代码离北极星多远。每轮迭代跑一遍。

### 5.1 IR 与 Schema
- Timeline JSON schema 有版本字段吗？演进规则定义了吗？
- 时间和帧率都是有理数吗？还有没有残留的 `double seconds`？
- 所有 effect / transform 参数都走统一的 animated property 结构吗？
- Effect kind 都有类型化参数 schema，还是有"裸 map"漏网？
- Asset 有 contentHash 且被缓存键采用吗？

### 5.2 C API 与跨语言
- 头文件在 `extern "C"` 保护里吗？C-only 客户端能编译吗？
- 暴露出去的类型都是不透明 handle 或 POD 吗？C++ 类型有没有泄露？
- C API 能被 JVM（JNI）、Kotlin/Native（cinterop）、Swift、Python 干净消费吗？

### 5.3 确定性
- 固定输入跑两次，输出字节相同吗？
- 跨平台同一 build 的软件路径输出相同吗？
- HW 编码器路径是否被显式标记为"不保证 bit-identical"且有 SW 回归路径？

### 5.4 缓存与增量
- 中间结果按什么键缓存？key 包含 content hash 吗？
- 单个 clip 改参数，缓存能只失效下游吗？
- 有 API 让宿主 introspect 缓存状态吗？

### 5.5 GPU 后端
- bgfx 抽象是否贯穿所有 render path？有没有直调 Metal / GL 的漏网路径？
- EffectChain 合并（把连续 effect 编成单 pass）实现了吗？
- CPU fallback 路径在且通过相同正确性测试吗？

### 5.6 许可证供应链
- 每个依赖在 ARCHITECTURE.md 有 license 标注吗？
- CI 是否验证 FFmpeg 的 build 无 GPL 组件？（本 repo 本身不 ship CI — 见 `docs/TESTING.md` "CI scope"；依赖 license 验证 = host 仓库（talevia）的 CI 责任。）
- 新增依赖的 PR 是否强制 license check？

### 5.7 性能 / 成本预算
- FramePool / CodecPool 的命中率是否被**测量**并在 example / test 里 dump 出来？"没看见就不知道"不是理由。
- 解码 / 编码吞吐是否有 benchmark 守护？回归超过 X% 是否有显式门槛？
- 单次渲染的内存峰值是否在现有 examples 里被观察（至少量级）？
- 公共 C API 的函数数量 / 结构体字段数是否在显式预算里？膨胀未被讨论 = 静默劣化。
- LGPL-clean build 的最终二进制 / link graph 尺寸是否被跟踪？无跟踪就等同"预算 ∞"。
- 同一 timeline 重复渲染是否有缓存命中率下界？（确定性 + 增量缓存缺一不可，见 §3.1 / §3.3）

### 怎么用这份 rubric

每轮迭代的流程：

1. 对照每一节的判断题，读当前代码给每项打分（有 / 部分 / 无）。
2. 找出离北极星最远、又能在短周期内闭环的 2-3 项，作为下一轮实现任务。
3. 优先补**一等抽象**，不要为某个具体 effect / 格式打 patch——patch 不会沉淀成系统能力。
4. 每轮结束，回到 §1 北极星重读一遍，校准方向没跑偏。

---

## 附：与 talevia VISION 的关系

talevia VISION 在 `/Volumes/Code/talevia/docs/VISION.md`。

- **talevia VISION**：整个创作系统的北极星——source/compiler/artifact 构建系统模型、DAG、AIGC 驯服、双用户张力
- **media-engine VISION**（本文）：Compiler 层里"传统引擎 + 特效渲染"子系统的北极星

下级服从上级——talevia 的"确定性、增量编译、DAG 缓存"直接塑造了本文 §3。反过来，本文的"理想 IR"会推动 talevia 的 Timeline 模型演进（加 keyframe、加 typed effect params、加 contentHash）。

**读序**：talevia VISION → 本文 → 本仓 `ARCHITECTURE.md` / `API.md` / `TIMELINE_SCHEMA.md` → 代码。
