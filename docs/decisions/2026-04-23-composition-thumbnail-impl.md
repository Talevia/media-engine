# 2026-04-23 — composition-thumbnail-impl

## Gap

`src/orchestrator/thumbnailer.{hpp,cpp}` 持有一个 `Thumbnailer` 类，stub 返回
`ME_E_UNSUPPORTED`。同时 `src/api/thumbnail.cpp` 下的公共 C API
`me_thumbnail_png(engine, uri, …)` 已经**完整**实装了"单文件 → PNG"的完整
管线（demux → seek → decode → sws → PNG encode），**完全绕过**这个 stub。

结果是"一个类两副面孔"的错位：

- Orchestrator 层的 `Thumbnailer` 按 "timeline 合成后取帧" 设计，构造函数
  吃 `shared_ptr<const Timeline>`。
- C API 的 `me_thumbnail_png` 按 "asset 级缩略图" 设计，入参是 URI，和
  Timeline 毫无关系。

一个名字 `Thumbnailer` 同时承载这两种语义，读代码的人看到 stub + 注释说
"单帧 → PNG" 会以为是全部的 thumbnail 路径——其实不是。PAIN_POINTS
2026-04-23 已经把这个错位记录下来了。

## 决策

**本轮不做 composition-level 实装**（M6 frame server 才有消费方），但把
类名从 `Thumbnailer` → `CompositionThumbnailer`，让"这是 *composition*-级
路径"在名字层就明确：

1. `src/orchestrator/thumbnailer.{hpp,cpp}` → `src/orchestrator/composition_thumbnailer.{hpp,cpp}`（`git mv`，保留 blame）。
2. 类名 `Thumbnailer` → `CompositionThumbnailer`。
3. 顶部注释改写：明确本类是 "timeline-driven, composition-level"；asset-level
   `me_thumbnail_png` 是**另一条独立路径**、不走本类；两者 stay separate。
4. STUB marker 的 slug 保持 `composition-thumbnail-impl`（本来就对，无需改）。
5. `src/CMakeLists.txt` 里源文件路径更新。
6. 文档同步：
   - `src/orchestrator/README.md` — 文件条目 + 一句 asset vs composition 的边界说明。
   - `docs/ARCHITECTURE.md` — module map 和当前状态表里的 `Thumbnailer::png_at`
     → `CompositionThumbnailer::png_at`。
   - `docs/ARCHITECTURE_GRAPH.md` — ASCII diagram、class snippet、§批编码
     里提到 "Previewer / Thumbnailer" 的一行，全部 rename。
   - `CLAUDE.md` — 架构不变量第 9 条的 "orchestrator/" 角色清单 rename，
     并加一句点出 asset-level thumbnail 不走本类。

## 被拒的替代方案

1. **合并两条路径为一个 `Thumbnailer(Timeline | URI)` 变体**——被拒：
   VISION §2 和 ARCHITECTURE_GRAPH.md 都写死 orchestrator 只对 Timeline
   求值。把 URI 塞进 orchestrator 会破坏"orchestrator 吃 Timeline / 只吃
   Timeline"的不变量。

2. **本轮顺带把 composition-level 实装出来**——被拒：唯一消费方是 M6 的
   frame server，frame server 本身还没存在；现在实装等于造一个没有
   consumer 的 surface，会给 M6 rename/重写留包袱。按 VISION §3.3 "先有
   consumer 再给 provider" 的原则，等 M6 再动。

3. **保留 `Thumbnailer` 类名不动，只改注释**——被拒：错位一半是文字、一
   半是类型。如果类型还叫 `Thumbnailer`，下一个读者看到 C API 没走它时
   仍然会怀疑 "是不是应该走"。rename 是成本最低的去歧义手段。

4. **同时建一个 `AssetThumbnailer` 类封装 C API 的实现**——被拒：C API
   现在就是 `src/api/thumbnail.cpp` 一个文件的事，~120 行自包含。多一
   层类包装没有抽象增益（单一 caller、单一入口），只会把简单 C→libav
   调用链拉长。等以后 asset thumbnail 有多个变体（动图、GIF、sprite
   sheet）再抽。

## 自查（§3a 设计约束）

- 类型化 effect 参数：N/A，不碰 effect 层。
- 浮点时间：N/A，类签名仍用 `me_rational_t`。
- 公共头：**不改** `include/media_engine/*.h`。
- C API 异常 / STL 泄露：N/A，本轮只动内部类。
- GPL：无新 CMake 依赖。
- 确定性：rename only，二进制 byte-equal。
- Stub 净增：**不增**——`check_stubs.sh` 前 3 条（`frame-server-impl` ×2
  + `composition-thumbnail-impl`），后 3 条完全一致，slug/note 都不变，
  只有一行文件路径从 `thumbnailer.cpp` → `composition_thumbnailer.cpp`。
- OpenGL：N/A。
- Schema 兼容：N/A。
- ABI：不动公共头，ABI 不变。

## 验证

- `cmake --build build` 全绿，`-Werror` clean。
- `ctest --test-dir build` 6/6 通过（test_status / test_engine /
  test_timeline_schema / test_content_hash / test_determinism /
  test_cache）。
- `./build/examples/01_passthrough/01_passthrough examples/01_passthrough/sample.timeline.json /tmp/out_c21.mp4`
  正常，输出 117195 字节。
- `./build/examples/06_thumbnail/06_thumbnail /tmp/out_c21.mp4 500 120 120 /tmp/thumb_c21.png`
  正常（这条路径**不**走 `CompositionThumbnailer`，走 `src/api/thumbnail.cpp`，
  正是本轮说明的边界），输出 2614 字节 PNG。
- `tools/check_stubs.sh` stub 总数 = 3，不变。
- `grep -rn 'class Thumbnailer' src docs` = 0（仅 `CompositionThumbnailer`
  一处类定义）；老名字只剩决策归档文件里讨论历史的引用，这是正确的。

## 影响 / 后续

- PAIN_POINTS 2026-04-23 的 "`Thumbnailer` 类签名与 C API 不对齐" 条目被
  此次 rename 消解一半——名字层消歧完成，**功能层**（真正实装
  composition-level 取帧）仍在 backlog `composition-thumbnail-impl` 等
  M6 frame server。
- M6 frame server 落地时，直接复用这个类名，加 graph 求值 + PNG 编码路
  径即可，无需再改名。
