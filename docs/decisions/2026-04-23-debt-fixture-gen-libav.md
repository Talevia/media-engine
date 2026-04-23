## 2026-04-23 — debt-fixture-gen-libav：test_determinism fixture 改用 libav 直写（Milestone §M1-debt · Rubric §5.2 + §5.3）

**Context.** `tests/test_determinism` 依赖一个 10 帧 320×240 MPEG-4 Part 2 MP4 作为 passthrough 的输入。之前 `tests/CMakeLists.txt` 用 `find_program(FFMPEG_EXECUTABLE ffmpeg)` 跑 `ffmpeg -f lavfi testsrc=...` 生成 fixture，找不到 `ffmpeg` on PATH 就打一条 status 消息让测试在运行期 skip。这给了 CI 一个静默后门：没有系统 ffmpeg CLI 的环境里，determinism 回归会被 skip 而非失败——`PAIN_POINTS.md` 2026-04-23 记录过这条。M1 的 exit criteria 要求"至少 1 条通过的 passthrough 确定性回归"，但带静默 skip 的实现不能算真达标。

**Decision.** 新增 `tests/fixtures/gen_fixture.cpp`，一个独立的 C++ helper，直接链 `libavformat / libavcodec / libavutil`（media-engine 本来就必链的依赖），把 fixture 编码逻辑 inline 在仓库里。`tests/CMakeLists.txt` 里：

- 删掉 `find_program(FFMPEG_EXECUTABLE ffmpeg)` 分支 + fallback 打印，改为**始终**生成 fixture。
- 新加 `add_executable(gen_fixture fixtures/gen_fixture.cpp)`，链 `FFMPEG::avformat/avcodec/avutil`。
- `add_custom_command` 用 `$<TARGET_FILE:gen_fixture>` 跑新 helper，替换原先的 `ffmpeg` CLI 调用。
- `tests/CMakeLists.txt` 顶部补一次 `find_package(FFMPEG REQUIRED)`，因为 FFmpeg 的 imported targets 是在 `src/CMakeLists.txt` scope 创建的，tests/ 目录看不到；`FindFFMPEG.cmake` 本身 `if(NOT TARGET ...)` 守过，多调一次安全。

Fixture 的 spec 保持与原 CLI 版本一致：320×240 YUV420P / 10 fps / 10 帧 / MPEG-4 Part 2 / MP4。MPEG-4 Part 2 是 LGPL libavcodec 必带的 encoder（libx264 是 GPL 才不能用），仍符合 `INTEGRATION.md` "ship builds must use LGPL FFmpeg" 的站位。`test_determinism.cpp` 里运行期检查 `ME_TEST_FIXTURE_MP4` 是否存在那段 skip 保留——作为 belt-and-suspenders；现在 CMake 侧无条件生成 fixture，该分支只在生成失败时才触发。

`gen_fixture` 的 determinism 策略三件套：`thread_count=1`（mpeg4 encoder 并行会让输出依赖 scheduling）、`AV_CODEC_FLAG_QSCALE + global_quality = FF_QP2LAMBDA*5`（等价于 `-q:v 5`，避免 rate-control 路径的非确定性）、`AV_CODEC_FLAG_BITEXACT` + `AVFMT_FLAG_BITEXACT`（encoder 不写 libav 版本串、mp4 muxer 的 `mvhd/tkhd.creation_time` 置 0）。实测两次 `gen_fixture /tmp/a.mp4` 和 `/tmp/b.mp4` 产物 SHA1 一致（`4924dfe5…`）——fixture 自身就 byte-identical，不只是 passthrough 结果 deterministic。

**Alternatives considered.**

1. **保留 CLI fallback，只把 skip 改成 `message(FATAL_ERROR)`**——强迫 CI 装 ffmpeg。拒：多一个 runtime dep（nix/CI 的 flake 增加），而我们已经链了 libavcodec，fixture 改走 libav 不多花成本；外部 CLI 的输出还受 ffmpeg 版本差异影响（FFmpeg 5.x/6.x/7.x/8.x 的 `-f lavfi testsrc` 默认生成参数微调过），自己代码控制更稳。
2. **把 fixture 直接 commit 成二进制** 进 `tests/fixtures/`——拒：二进制 checked-in fixture 是常见反模式（git 膨胀、更新时 diff 不可读、跨 host 字节序 / libav 版本差异的修复路径不透明）。当前 4×4 个 MP4 字节 ≈ 27 KB 不大，但往后 reencode/thumbnail 测试都会要 fixture，全 commit 到 repo 会滚雪球。
3. **在 CMake 里用 `configure_file` + 一段 Python/Perl 脚本生成 raw YUV 再走 ffmpeg**——拒：增加 Python runtime dep 比 libav 路径还糟。
4. **把 `gen_fixture` 做成 CMake `install()` 的 host tool**——拒：过度设计。这是 test-only helper，`ME_BUILD_TESTS=ON` 才编。

业界共识来源："libav BITEXACT 组合"在 FFmpeg 8.1 的 `libavformat/movenc.c` 和 `libavcodec/options_table.c` 源里是 mp4 muxer 和 encoder metadata 稳定化的标准路径；FFmpeg 自己的 fate regression test harness 就是走 `-fflags +bitexact` 路线跑 byte-identical 回归。

**Coverage.**

- `test_determinism` 现在无条件跑（2 test cases, 10 assertions, 0 skipped），`ctest --test-dir build` 全 6 suite 绿。
- `01_passthrough` 例子用 `sample.timeline.json` 验证仍能端到端产出 2 秒 MP4（回归检查，确保没碰到 passthrough 主路径）。
- `gen_fixture` 自身的 byte-identical 产物手工复现确认（两次 invoke，SHA1 相同）——不入 ctest 以免把 encoder 细节 lock 在测试里，但验证了 BITEXACT 策略生效。

**License impact.** 无新依赖。`FFMPEG::avformat / avcodec / avutil` 已经是 `src/` 的 PRIVATE link；`tests/` 只是显式再 `find_package(FFMPEG REQUIRED)` 进本 scope，链到同一套 LGPL library。

**Registration.** 无 C API / schema / kernel 注册点变更。CMake 侧动了：
- 新 CMake target `gen_fixture`（tests/ 内部，不 `install()`）。
- `tests/CMakeLists.txt` 多一处 `find_package(FFMPEG REQUIRED)`（作用域 tests/，与 src/ 的 find_package 独立）。
- `add_custom_command` DEPENDS 从 `${CMAKE_CURRENT_LIST_FILE}` 扩展到 `gen_fixture ${CMAKE_CURRENT_LIST_FILE}`——gen_fixture 源码改动会触发 fixture 重生。
