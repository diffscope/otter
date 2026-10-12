# Linux 与 macOS 构建审计，以及 CI 的 macOS 覆盖

> 快照：HEAD `e770ab9`（2026-10-06）。本文的行号引用在后续提交后必须自行复核。
>
> **落地形态与已知偏差**：本轮的 4 个本地提交在推送前合并为 2 个——`95e58bf`（`CliProcess.cpp` 的信号修复）与 `df34f7e`（CI + 本文档，已推送到 `origin/spec2.4-uptake`，触发 run #13）。合并时按路径分阶段提交，但 `ci.yml` 在第二个提交里已被整文件提交，原计划的第三个提交因此成为空提交而未能生成：**D5 的触发器改动实际落在 `df34f7e` 内，而该提交的信息只描述了 macOS 作业，没有提到触发器收紧**。内容本身正确（推送后 `HEAD^{tree}` 与合并前逐字节一致，`766d904`），但这条提交信息与它携带的改动不完整对应。修复需要改写已推送的历史（force push），本轮未做。

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

### P0（新，run #13 暴露并已修）：macOS 作业缺一个 Python 依赖安装步骤

+ **成因已定论**：`.github/workflows/ci.yml` 里安装 `onnx`/`numpy` 的步骤原有两个互斥分支——`Install build tools` 为 `if: runner.os == 'Linux'`，`Install the Python build tools` 为 `if: runner.os == 'Windows'`。**macOS 两个都不满足，因此该作业从未安装 onnx**，这是一处条件分支漏项，不是平台问题。
+ 后果链：macOS runner 镜像自带 Python → `find_package(Python3)` 命中它 → 配置期 `import onnx, numpy` 失败 → `_otter_can_make_fixtures` 为 OFF → 四个模型测试被 `DISABLED` → `Require the model tests` 打印 `test_Rmvpe is not an enabled test, so an interpreter without onnx was found` 并转红。
+ 归属层：**本仓的 workflow**。`synthrt`、vcpkg、macOS 全部无关——同一次运行里 `Install dependencies`、`Configure`、`Build` 都是成功步骤。
+ 修复（已实施，见 §4 的 D7）：新增 `Install the Python build tools on macOS`（`if: runner.os == 'MacOS'`），当场 `pip install --user onnx numpy` 并**断言 `import onnx, numpy` 成功**——这样「解释器没有 onnx」在十几秒内暴露，而不是等一次完整构建之后。
+ 教训（同类缺陷的形状）：凡是按 `runner.os` 分支的安装步骤，**新增平台时必须逐个复核每个分支是否覆盖新平台**。本仓的门禁哲学是「宁可早失败也别让套件无声缩水」，而这次正是门禁替 CI 抓到了这个漏项——但代价是十几分钟才报。

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
| D4 | 三个检查步骤（夹具生成、`Require the model tests`、消费者包校验）在 macOS 上原样复用，不新增；**但「安装 Python 依赖」不在复用之列**（见 D7，它是按平台分支的，macOS 当时被漏掉） | 保持三平台一致；`Require the model tests` 在 macOS 上仍能把「ONNX 特性丢失导致测试静默消失」变成红——run #13 证明它确实报了 | 本方案（D7 为 run #13 后修正） |
| D5 | 触发范围改为 `branches: ["spec2.4-**"]`，并去掉 `pull_request` | 让 2.4 线上的 push 直接跑到新作业；代价是其它分支不再被 CI 覆盖，且这条线将来开 PR 时检查不会自动挂上 | 用户确认（「目前只在 2.4 这个分支上提交和测试」） |
| D6 | 信号消费保留平台分支：Darwin 走 `sigpending()+sigwait()`，其余 POSIX 保持 `sigtimedwait()` | 这是用户明确选定的一项；代价是同一段逻辑有两份实现，注释必须写明为何不统一。另一种更少分支的写法是两平台都用可移植的 `sigpending()+sigwait()`，但那会改动 Linux 一条已跑通且没有测试兜底的运行时路径，不属于本轮最小改动 | 用户确认 |
| D7（run #13 后追加） | 单独加一个 `if: runner.os == 'MacOS'` 的 Python 依赖步骤（含 `import onnx, numpy` 断言），而不是把 Windows/Linux 两步改成 `!=` 之类的合并写法 | 保留另两平台已验证的步骤内容、改动最小，且平台名在标签里显式可见；代价是同一件事有三处分支，将来再加平台仍要逐个复核 | 本方案（依据 run #13 的失败读数） |

## 5. 未覆盖项与建议

1. **SIGPIPE 消费路径没有测试。** 现有用例（`test_GameGgml.cpp`、`test_TifaGgml.cpp`）不构造「子进程已退出后再写入」的场景，所以修复只被编译覆盖。建议后续补一个用例：spawn 假 CLI → `terminate()` → 再 `write()`，断言返回错误而不是进程被 SIGPIPE 杀死。
2. **Intel macOS 不可用**，见「需人工确认的关键点」第 1 条。
3. **`Require the model tests` 的时机偏晚**：run #13 印证了这一点——平台缺件要等一次完整构建才报（十几分钟），而同样的缺件在安装步骤里十几秒就能查出。是否把「解释器必须有 onnx」提前到配置期（本仓的包测试同样是配置期决定启停），留待下一轮决定；本轮只在自己的 macOS 步骤里加了断言，没有动既有门禁。
4. **与 CI 无关的本机事实**：本地 `build/`、`.tmp/`、`.safety/` 是既有产物目录，均已在 `.gitignore` 内；`docs/plans/asr-integration.md` 为未跟踪的历史遗留文件，本轮未触碰。

## 6. 首跑实测（run #13）

推送 `df34f7e` 到 `origin/spec2.4-uptake` 后触发 run #13（[run #13](https://github.com/diffscope/otter/actions/runs/38230044049)）：触发器改动确实生效——**该分支上真的跑起来了**，D5 的目的达成。

| 作业 | 结论 | 说明 |
| :-- | :-- | :-- |
| `build (ubuntu-24.04, x64-linux)` | **success** | 与 run #12 一致，改动未使 Linux 回归 |
| `build (windows-2022, x64-windows)` | **success** | 未回归 |
| `build (macos-15, arm64-osx)` | **failure** | 失败步骤 = `Require the model tests`（唯一的 failure 步骤；`Configure`/`Build` 均通过） |

**这是本轮最重要的读数，且方向与预期相反：** macOS 的 `sigtimedwait` 编译错误**确实被修复了**——`Configure` 与 `Build` 两步都成功，说明 `CliProcess.cpp` 与整个仓库在 macOS arm64 上编得过；同时它**推翻了我先前把「synthrt 能否在 macOS 编译」列为最大风险的判断**（该风险未发生）。失败的是那份防沉默门禁本身，成因见 §2 的 P0（新）。

顺带把两条待验证项升级为已证：`scripts/vcpkg` 覆盖三元组 `arm64-osx` 在 macOS arm64 runner 上确被采用（否则三元组缺失元数据会 FATAL_ERROR，而 `Install dependencies` 是成功步骤），`synthrt` 及其 macOS 依赖臂（`stdcorelib`、`qmsetup`、`bit7z`/`7zip` 等）也都在 macOS 上真的构建成功了。

**待补**：本节记录的是 **run #13（`df34f7e`）**的读数；`Install the Python build tools on macOS` 修好之后的**第二次实测（run #14 及以后）另开一节**记录，届时才会知道 macOS 是否全绿，以及 pip 在 macOS runner 上是否真的装得动 onnx。



