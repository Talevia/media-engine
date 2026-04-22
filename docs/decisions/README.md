# Decisions

每次 `iterate-gap` 完成一个 gap，在这里新建**一个文件**归档决策。**不要** append / edit 已有文件——新文件才能被 `ls` / `grep` / 跨文件聚合干净。

## 文件命名

```
docs/decisions/<YYYY-MM-DD>-<slug>.md
```

- 日期用当天（本地即可），与 commit 日期对齐。
- `<slug>` 复用处理的 backlog bullet 的 slug。
- 同日同 slug 冲突 → 加后缀 `-2`、`-3`。

## 模板

```markdown
## YYYY-MM-DD — 短标题（Milestone §Mx · Rubric §5.y）

Commit: `<shorthash>`

**Context.** 这个 gap 为何是本轮挑出来的——对应的 rubric 轴 / milestone 状态 / 当前代码里观察到了什么。

**Decision.** 落地了什么。关键类型名、函数名、文件路径。

**Alternatives considered.** 至少两个。每个：做了什么 + 为何被拒。
「业界共识」要具体到名字才算数（如「FFmpeg passthrough 模式的 pkt->pos=-1 约定」、
「nlohmann::json SAX vs DOM」、「bgfx EffectChain compile-to-single-pass 模式」）。

**Coverage.** 哪些测试 / 例子覆盖了这个改动。

**License impact.** 是否引入新依赖？新依赖的 license 是否在 ARCHITECTURE.md 白名单里？
（纯代码改动写「无新依赖」。）
```

## 什么不该进这里

- 实现细节（在代码和注释里）
- 运维笔记（在 CLAUDE.md / ARCHITECTURE.md 里）
- TODO 清单（在 BACKLOG.md 里）
- 愿景讨论（在 VISION.md 里）

这里只记**本轮做了什么选择、为什么是这个选择、拒了什么选择**。
