# Linux 与 macOS 构建审计，以及 CI 的 macOS 覆盖

> 快照：HEAD `e770ab9`（2026-10-06）。本文的行号引用在后续提交后必须自行复核。

## 需人工确认的关键点

+ **Intel macOS（x64-osx）本轮不覆盖，且当前无法覆盖。** `onnxruntime-builds` 端口在 Darwin 上只接受 arm64（`scripts/vcpkg/ports/onnxruntime-builds/portfile.cmake:29-32` 硬失败，`vcpkg.json` 的 `supports` 为 `!(windows & !x64) & !(osx & !arm64)`）。上游**也无包可取**：对 `v1.24.4` 的 release 资产做 HEAD 探测，`onnxruntime-osx-arm64-1.24.4.tgz` 与 `onnxruntime-linux-x64-1.24.4.tgz` 均 302（存在），而 `onnxruntime-osx-x86_64-1.24.4.tgz`、`onnxruntime-osx-universal2-1.24.4.tgz` 均 404（不存在）。要覆盖 x64 macOS，先得改上游端口或改为源码构建 ONNX Runtime——属于另一个量级的工程。
+ **macOS 依赖臂只有 arm64 一种已验证路径**，因此 CI 的 macOS 项必须跑在 arm64 镜像上（本方案取 `macos-15`）。若将来换成 x64 镜像，默认三元组会退成 `x64-osx`，`vcpkg install` 会直接失败。
+ **`sigtimedwait` 缺陷的修复没有测试覆盖。** 现有 ggml 测试不触发「向已死子进程写入」这条路径（见下文第 5 节），所以 CI 只能证明它**编译通过**，不能证明运行时语义正确。若要覆盖，需要新增一个针对该分支的用例。
+ **触发范围已收紧到 2.4 这一条线**（`branches: ["spec2.4-**"]`，并去掉 `pull_request` 触发器），依据是「目前只在 2.4 分支上提交和测试」。副作用有二：其它分支上 push 不再跑 CI；这条线将来开 PR 时，检查不会自动挂到 PR 上，需要把 `pull_request` 加回来。

## 1. 结论

+ 本项目的 Linux 构建**已通过 CI 验证**：`.github/workflows/ci.yml` 的矩阵由 `3b7ec31`（2026-10-03）引入时就包含 `ubuntu-24.04`（`x64-linux`），最近一次运行（run #12，HEAD `e770ab9`）为 success。因此「加一份 CI」的实际缺口是 **macOS**。
+ macOS 构建当前**必然失败**，根因与 issue #2 一致：`CliProcess.cpp:320` 调用 Darwin 不提供的 `sigtimedwait()`（issue 正文写的 `:303` 与本地快照不符，以本仓代码为准）。
+ 除该处外，未发现其它 macOS 专属的编译阻塞点——全仓只有两个文件带平台分支（`CliProcess.cpp`、`tests/auto/Analysis/fakecli.cpp`），其余代码无 `__linux__`/`__APPLE__` 之类的平台假设。

## 2. 缺陷清单

### P0：`sigtimedwait()` 在 macOS 未声明（issue #2）

+ 位置：`src/plugins/inferenceinterpreters/external/CliProcess.cpp:320`（**修复前**的 `finish` lambda，即 `sigtimedwait` 循环所在行）；修复后该 lambda 位于 `:302-324`，其中 `__APPLE__` 分支 `:303-314`、`#else` 分支 `:315-322`。
+ 事实：macOS 15.8 的 man 页中 `sigtimedwait(2)` **不存在**（`sigwait(2)` 存在，SECTION 2，声明于 `<signal.h>`）。Linux/glibc 则在 `-std=gnu++17`（`_GNU_SOURCE`）下提供它。这是两平台的实质差异，不是头文件包含问题。
+ 影响面：`otter_cli_support` 这个静态库在 `add_library` 处**无条件构建**（`src/plugins/inferenceinterpreters/CMakeLists.txt:118`，不在 `OTTER_BUILD_GAME_GGML`/`OTTER_BUILD_TIFA_GGML` 两个开关内），所以 macOS 上任何配置都会编到这段代码——即使两个 ggml provider 都关掉，构建依然失败。
+ 修复：按 issue 建议改为 `sigpending()` + `sigwait()`；Linux 保持 `sigtimedwait()` 路径不变。

### P1：`pthread_sigmask` 依赖传递包含

+ 位置：同文件 `:300`、`:323`（两处 `pthread_sigmask`），头文件位于 `:26`。
+ 事实：原始文件未包含 `<pthread.h>`。glibc 的 `<csignal>` 会带入声明，所以 Linux 能编过；但这是偶然的传递包含，与上一个提交（`e770ab9`）修掉的 `<fcntl.h>`/`<cerrno>` 属同一类缺陷。
+ 修复：POSIX 分支补 `#include <pthread.h>`。

### 未证实（不作为缺陷排期）

+ `synthrt` 在其 pinned 提交（`63bef253607649dcface0b37abc68f051cc45c8b`）上能否在 macOS arm64 编译通过。这是上游仓库的构建能力，本地无法验证；由新增的 macOS CI 首跑给出答案。已有的间接证据：其 vcpkg 端口对非 Windows 平台显式把 DirectML 关掉、改让 dsinfer 走 CPU provider（`scripts/vcpkg-ports/synthrt-main/portfile.cmake:34-50`），说明该分支确实考虑过非 Windows 平台。
+ 未覆盖的运行时分支：向已死子进程写入时的 SIGPIPE 消费路径（见第 5 节）。

## 3. 逐项审计结果

| 项 | 结论 | 依据 |
| :-- | :-- | :-- |
| CI 现状 | Linux + Windows 已覆盖，macOS 缺失；触发范围原为 `[main, "analysis-**"]` + 全部 `pull_request`，现收紧为 `spec2.4-**` | `.github/workflows/ci.yml` 矩阵与 triggers；run #12 success |
| 三元组 | `x64-linux`、`arm64-osx` 均在 `scripts/vcpkg/triplets/` 内，且都把 `VCPKG_LIBRARY_LINKAGE` 覆盖为 dynamic（内建三元组是 static） | `scripts/vcpkg/triplets/*.cmake` |
| ONNX Runtime 载荷 | Linux x64 与 macOS arm64 的 SHA512 都已登记；macOS 走 `onnxruntime-osx-arm64-<ver>.tgz`，`.dylib`/`.dSYM` 由 `_deploy_runtime_contents` 分流。该包是 Microsoft 官方预编译 release，不由本仓构建 | `scripts/vcpkg/ports/onnxruntime-builds/setup-onnxruntime/{hashes,setup}.cmake` |
| 测试运行时搜索路径 | 已同时设置 `PATH`/`LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` | `src/tests/auto/Analysis/CMakeLists.txt:64-68` |
| 安装面校验 | 库名正则与插件正则都已含 `dylib`（**推断**：这是代码对该名称的支持，未经 Darwin 实跑）；`otter_support` 是静态库，不安装 | `src/tests/auto/Analysis/check-installed-surface.cmake:71,85` |
| 平台分支代码 | 仅 `CliProcess.cpp` 与 `fakecli.cpp` 带 `_WIN32` 分支；测试内部把 `\` 归一化为 `/` | 全仓 grep `_WIN32|__APPLE__|__linux__|_MSC_VER|__MINGW` |
| 依赖的平台支持 | `bit7z`/`7zip`/`blake3`/`sparsepp` 的 `supports` 字段缺省、`boost-test` 为 `!uwp`，都不排斥 macOS/Linux；它们来自 vcpkg 内建 registry（pinned commit `1fddddc`） | 各端口 `vcpkg.json`（逐个 fetch 核对） |
| Windows 专属 CMake | `qm_add_win_rc` 与 `CMAKE_DEBUG_POSTFIX` 都在 `WIN32` 分支内 | `src/conf.cmake:15-21`、`CMakeLists.txt:21-39` |

## 4. CI 设计（已确认的决策）

| 编号 | 决策 | 取舍 | 来源 |
| :-- | :-- | :-- | :-- |
| D1 | 扩展现有 `ci.yml`，加一个 macOS 矩阵项，而不是新建 workflow 文件 | 与另两平台共用同一套步骤，改一处即可；代价是平台差异要靠 `runner.os` 条件控制 | 用户确认 |
| D2 | macOS 用显式的 `macos-15`（arm64 镜像） | `macos-14` 按 GitHub 公告于 2026-11-02 退休；`macos-latest` 指向的 `macos-26` 同时存在 x64 与 arm64 两种镜像，而本构建要的正是 arm64，所以钉显式版本而不是浮动标签 | 本方案（GitHub changelog，2026-10-01） |
| D3 | macOS 默认三元组即 `arm64-osx`，不额外传 `--triplet`/`--overlay-triplets` | 少一处配置；代价是依赖「镜像必须是 arm64」这一前提，已在 workflow 注释中写明 | 本方案 |
| D4 | 不新增任何步骤：Python/onnx 夹具生成、`Require the model tests`、消费者包校验三步在 macOS 上同样适用 | 保持三平台一致；`Require the model tests` 在 macOS 上仍能把「ONNX 特性丢失导致测试静默消失」变成红 | 本方案 |
| D5 | 触发范围改为 `branches: ["spec2.4-**"]`，并去掉 `pull_request` | 让 2.4 线上的 push 直接跑到新作业；代价是其它分支不再被 CI 覆盖，且这条线将来开 PR 时检查不会自动挂上 | 用户确认（「目前只在 2.4 这个分支上提交和测试」） |
| D6 | 信号消费保留平台分支：Darwin 走 `sigpending()+sigwait()`，其余 POSIX 保持 `sigtimedwait()` | 这是用户明确选定的一项；代价是同一段逻辑有两份实现，注释必须写明为何不统一。另一种更少分支的写法是两平台都用可移植的 `sigpending()+sigwait()`，但那会改动 Linux 一条已跑通且没有测试兜底的运行时路径，不属于本轮最小改动 | 用户确认 |

## 5. 未覆盖项与建议

1. **SIGPIPE 消费路径没有测试。** 现有用例（`test_GameGgml.cpp`、`test_TifaGgml.cpp`）不构造「子进程已退出后再写入」的场景，所以修复只被编译覆盖。建议后续补一个用例：spawn 假 CLI → `terminate()` → 再 `write()`，断言返回错误而不是进程被 SIGPIPE 杀死。
2. **Intel macOS 不可用**，见「需人工确认的关键点」第 1 条。
3. **`synthrt` 能否在 macOS 编译通过仍未证实**，见「未证实」一节；macOS 作业首跑若红，应先读该结论再判断责任层。
4. **与 CI 无关的本机事实**：本地 `build/`、`.tmp/`、`.safety/` 是既有产物目录，均已在 `.gitignore` 内；`docs/plans/asr-integration.md` 为未跟踪的历史遗留文件，本轮未触碰。
