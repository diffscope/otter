# otter 测试包不入库：方案与决策台账

> 一句话：把 `src/tests/auto/Analysis/packages/` 下的 13 个测试包（28 个文件、13.9 KB）移出 git，改由生成器重建，并以「生成物 == 删除前提交的记录字节」作为等价判据；实现形态有三种候选（§3），其中 **D-OTP1** 待选定后再动手。
> 快照：otter `HEAD = c1c8971`（分支 `analysis-level-1`），2026-10-03。工作区唯一未跟踪文件是他人留下的 `docs/plans/asr-integration.md`，本方案不触碰它。
> （2026-10-03 二次压缩后：本文引用的增量 SHA 已不在分支上；当前增量以 `git log` 的提交主题为准，按「提交主题 + 文件路径」定位而不是按 SHA。**订正**：这里原先写明的锚点 `backup/pre-squash-20261003-otter` 在本机不存在；现行锚点是三次压缩后的 `backup/pre-squash-20261003b-otter`。）
> 与前两阶段的关系：synthrt 已完成收敛与全量验证（145/145 构建 + ctest 16/16），wolf 已完成同类改造（9 笔提交、门禁 17/17 全绿）。本方案是同一目标在 otter 上的收口，**本次只做 otter**。

## 0. 需人工确认的关键点

| 编号 | 决策 | 结论（R4 已确认） |
| :-- | :-- | :-- |
| **D-OTP1** | 13 个测试包的**去向形态** | **已定：移出 git，改为外部数据目录；目录缺失时两条装载测试以退出码 77 跳过。** 因为 release 里没有合成负例可下载，交付一个 **CMake 脚本**（`scripts/make-test-packages.cmake`，`cmake -P` 运行）从表重建这批包，本地与 CI 都用它把目录准备好 |
| **D-OTP2** | 新增机制是否引入 Python | **已定：不引入。** 生成与下载都用 CMake 脚本（`file(WRITE)` / `file(DOWNLOAD)`），仓内已有 `check-installed-surface.cmake` 先例；banned 的是新增 Python 依赖，不是清理既有 Python |
| **D-OTP3** | 验收范围 | **已定：纳入「数据缺失 → 退出码 77 跳过」路径的实测** |
| **D-OTP6** | 模型夹具是否改从 release 下载 | **已定：新增非 Python 下载脚本（`scripts/fetch-models.cmake`，`cmake -P`），按 release 的 `manifest.json` 校验 SHA512 拉取四个真实包；现有合成夹具与其断言不动**（合成夹具仍是 CI 与快速门禁的基础，真实包体积数百 MB） |
| **D-OTP7** | CI 保障 | 推荐：CI 里加一步运行生成脚本，并仿照既有「四个模型测试必须注册」的守卫，防止两条装载测试静默变成跳过（实施时一并落地） |

选 CMake 脚本 + 外部目录的理由：本仓构建已有 Python 依赖是事实（模型夹具），但按 D-OTP2 不得再引入；CMake 是构建既有依赖，脚本模式无需配置、无需联网即可重建 13.9 KB 数据，并在本机实测过与删除前记录字节逐字节一致（§4）。

## 1. 现状（事实，逐条可复查）

1. 13 个测试包共 28 个文件、13.9 KB，位于 `src/tests/auto/Analysis/packages/<name>/{desc.json,inferences/*/inference.json}`，全部由 `git ls-files` 跟踪；最大的包 `importing`、`importing-with-options` 各 3 文件 2.2 KB，最小的 `no-declaration` 1 文件 0.2 KB。
2. 消费者只有两个测试：`test_AnalysisLoad.cpp`（293 行，11 处 `packages()` 调用，`:31-33` 由 `OTTER_TEST_PACKAGE_DIR` 宏得到根目录）与 `test_AnalysisRuntime.cpp`（892 行，1 处 `:61` 直接用宏）。宏在 `src/tests/auto/Analysis/CMakeLists.txt:70` 定义为 `"${CMAKE_CURRENT_SOURCE_DIR}/packages"`，即**源码目录绝对路径**。
3. 两个测试**本身已经在运行期往临时目录写包**：`test_AnalysisLoad.cpp:53-66` 的 `writeAligner()` 用 `std::ofstream` 拼 `desc.json` 与 `inference.json`，`test_AnalysisRuntime.cpp:98-102` 同类。形态 C 因此不是新机制，而是把已有做法统一。
4. 包 id 不能从目录名机械推出：12 个是 `otter/test-<目录名>` 一类，但有三个例外——`bad-exports-knob-range` 的 id 是 `otter/bad-exports-knob-range`（无 `test-`）、`unknobbed` 是 `otter/unknobbed`、`slow-analyzer` 是 `otter/test-slow`。生成器的表必须逐包记录 id，不能推导。
5. 本仓已有**同类先例**：模型夹具由 `scripts/make-model-fixtures.py` 在构建期生成到 `${CMAKE_BINARY_DIR}/fixtures`，用 `.stamp` 标记完成、缺失时用 `OTTER_TEST_SKIP_EXIT_CODE=77` 跳过、无 Python+onnx 时把 4 个模型测试注册为 `DISABLED`（`CMakeLists.txt:76-137`）；`TestSupport.h:93-107` 的 `FixturesOrSkip` 负责跳过判定。
6. CI（`.github/workflows/ci.yml`）在 Linux 与 Windows 两平台跑 configure/build/ctest，并且**专门有一道守卫**：`ctest -N` 里必须列出 `test_Rmvpe/test_Game/test_Hfa/test_Tifa`，否则报错退出（理由写在注释里：静默缩水的套件比失败的套件更糟）。ctest 共注册 12 条（10 个 `add_auto_test` + `test_CheckDeclarations` + `test_InstalledSurface`），与 `docs/OtterDesign.md:990` 记载一致。
7. `scripts/check-declarations.py` 是**发布前**的声明 lint，不消费这些测试包（`git grep` 无引用）；`scripts/make-package.py` 是发布装配器（声明 + 模型 → zip），与测试包无关。
8. 本机其它仓的依赖**不影响** otter 的门禁：otter 的 `build/vcpkg_installed` 里 synthrt 是新鲜的（`include/synthrt/SVS/SingerContrib.h`、`InferenceContrib.h` 都含 `NAME`），umbrella boost 已装（`share/boost/BoostConfig.cmake`），Python 带 onnx 1.19.1；其端口 `scripts/vcpkg-ports/synthrt-main/portfile.cmake` 的 `REF` 与本轮 synthrt 的 pin 一致（当时为 `f2f0f8ee…`，2026-10-03 synthrt 推送后随分支 tip 改为 `63bef25…`；**pin 的 sha 只以该端口文件为准**，此处不复写第二份）。**无需 wolf 那种影子安装根。**

## 2. 判据（怎么算做对）

1. **等价**：生成器产物必须与删除前提交里记录的**对象库字节**一致（比对用 `git cat-file blob`，**不看工作区**——`core.autocrlf` 会把工作区文件换成 CRLF）。**实测订正**：CMake 的 `file(WRITE)`/`file(CONFIGURE)` 在 Windows 上按文本模式写入、把 LF 变成 CRLF，`file(GENERATE)` 在 `-P` 脚本模式下不可用（2026-10-03 在 cmake 4.3.1 上实测），因此判据落地为**换行归一化后的逐字节等价**：内容与记录完全一致，仅行末随平台取值。记录本身 0 个 CR（28 个文件逐一核过），脚本载入表时会剥掉表里的 CR，使脚本自身被检出成 CRLF 时也不把 CR 带进产物。两处测试原本就用 `std::ofstream`（文本模式）写运行时包，JSON 读取器对 CRLF 不敏感，故该差异不影响测试语义。
2. **`--check` 自证**：在未改动代码上运行 `--check <dir>` 必须 0 差异（沿用 wolf 的自证纪律）。
3. **门禁转绿**：重新配置 + 全量构建 + ctest 全绿，且**不出现新增的跳过**；测试注册数不变（12 条）。
4. **缺数据行为明确**：按 D-OTP3 的结论实测跳过路径或显式声明不测。
5. **脚本自测**：生成器带 `unittest`（`scripts/test_make_test_packages.py`，随 `test_CheckDeclarations` 一起被 ctest 执行），覆盖表完整性、确定性、少文件/多文件/改内容三类差异、`--check` 判定、纯 LF 产物。**（订正，见 §3.4）**：选定方案改为 CMake 生成器后，仓库里不存在 `scripts/test_make_test_packages.*`，生成器的自证是用一次性工具手工做的，不由 ctest 承载。
6. **文档同步**：`docs/plans/HfaAlign.md:267` 提到 `src/tests/auto/Analysis/packages/*` 的那一行、以及 `docs/OtterDesign.md` 的测试注册数记载，必须与改造后的现实一致。

## 3. 候选方案

> 下文三种候选是 R4 探索时的选项记录，**最终选定 §3.4**（外部目录 + CMake 脚本 + 缺数据跳过 77）。

### 3.4 选定方案（R4 确认）

- **交付物一**：`scripts/make-test-packages.cmake` —— 内嵌 13 个包 / 28 个文件的表，`cmake -P` 运行；支持 `-DOTTER_TEST_PACKAGES_OUTPUT=<dir>`（写入）、`-DOTTER_TEST_PACKAGES_CHECK=<dir>`（逐字节校验该目录并报告 missing/differs/unexpected）、`-DOTTER_TEST_PACKAGES_LIST=ON`（只列表）。
- **交付物二**：`scripts/fetch-models.cmake` —— 从 `models-v0.1` 下载 `manifest.json` 与四个真实包（可用 `-DOTTER_FETCH_VARIANTS=` 只取部分），按 manifest 中的 SHA512 校验后放入指定目录；**不联网不在 CI 里默认执行**，由用户/开发者显式调用。
- **交付物三**：测试接线 —— 新增缓存变量 `OTTER_TEST_PACKAGES_SOURCE`（默认空）；两条装载测试经环境变量读取该目录。**语义是两级、不是一条**：变量未给或为空 ⇒ 打印生成命令并以 `OTTER_TEST_SKIP_EXIT_CODE=77` 退出（ctest 侧 `SKIP_RETURN_CODE 77` 记为跳过）；**变量给了但目录不存在 ⇒ configure 期硬失败**，不静默跳过。（R5 三条实测：未给 → exit 77；给了却不存在 → exit 1；置空后 ctest 报 `***Skipped` 但仍返回 0。）
- **交付物四（R5 实现 + 现状订正，2026-10-03）**：CI 在 Configure 前加「生成 + `--check`」两步（`9126ced`），并把生成目录传给 Configure；**守卫现状**：`ci.yml` 的 `Require the model tests` 只要求四个模型测试处于启用态，**没有**为两条装载测试加同类守卫。装载测试始终构建、只在运行期缺数据时跳过，要把「静默跳过」也挡住需另查 ctest 输出（候选改进，未实施）。
- **删除**：`src/tests/auto/Analysis/packages/` 下 28 个跟踪文件（单独提交）。
- 脚本自测：**不由 ctest 承载**——仓库里不存在 `scripts/test_make_test_packages.*`，ctest 注册的 12 条用例中也没有生成器自测。R4 的自证是用一次性工具手工做的（写入 / `--check` / 三类污染 / `--list`，见下条记录）；**常设防线是 CI 的 `--check` 步骤**（生成器表漂移会让 CI 失败），但它不覆盖「三类污染是否仍能被检出」。若要长期守卫，应在 `scripts/` 加一份 `test_*.py` 或一条 ctest 用例（候选改进，未实施）。
- **已完成的实测（R4）**：`scripts/make-test-packages.cmake` 已提交（`dae4f75`，770 行），并做过一轮自证：(a) 写入 28 个文件后与 `HEAD` 记录**归一化字节 0 不一致、0 多余**；(b) `--check` 对完好目录 exit 0；(c) 追加一字节 / 多一个文件 / 少一个文件三类污染**全部被检出**（均非 0 退出）；(d) `--list` 列出 28 个名字；(e) **把脚本自身转成 CRLF 后（773 个 CR）直接 `cmake -P` 写入，产物仍与记录 28/28 一致、`--check` 仍 exit 0**——即 Windows 检出场景成立。
- **已完成（R5，提交 `aed42c8`…`fab50fb`）**：接线与删除已落地——`aed42c8` 把两条装载测试改为从 `OTTER_TEST_PACKAGES_SOURCE` 读目录（**未给变量即退出 77 跳过；给了却不存在即失败**，不静默缩水），ctest 侧设 `SKIP_RETURN_CODE` 与 `ENVIRONMENT`；`TestSupport.h` 的跳过码宏带 `#error` 守护，6 个包含它的目标（2 装载 + 4 模型）都已由 CMake 定义；`2d3ab85` 删除 28 个跟踪文件（−569 行，`src/tests/auto/Analysis/packages/` 跟踪数归零）；`9126ced` 在 CI 加「生成 + `--check`」步骤并把目录传给 Configure（YAML 已解析复核，12 步）；`fab50fb` 同步 README 与 `docs/plans/HfaAlign.md:267`。
- **门禁已通过（R5 后半，用户授权后执行）**：全新 `build/cmake` + Ninja + MSVC 19.51（69 个构建步）→ `ctest`：**12/12 passed, 0 failed**（`test_AnalysisLoad` 0.08 s、`test_AnalysisRuntime` 15.62 s，两条都是 Passed 而非 Skipped，总 20.57 s；模型夹具在配置期生成成功）。跳过路径三条实测：变量未给 → 打印生成命令并 **exit 77**；变量指向不存在的目录 → **exit 1**（不静默跳过）；变量置空后 `ctest -R` 两条测试报 `***Skipped`，而 **ctest 仍返回 0** 并打印 "100% tests passed … 2 tests did not run"。最后一条是本方案最需要写明的坑：**跳过在 ctest 里是绿的**，因此 CI 必须自己保证数据齐备——这正是 CI 跑「生成 + `--check`」而不是只跑 ctest 的理由。实测后把变量指回生成目录，两条测试恢复 Passed。
- **已知重约束（R5 读到）**：`docs/Packages.md:7` 记载 rmvpe 单模型 345 MB、四个包合计数百 MB ⇒ `fetch-models.cmake` 必须**默认只列不拉**、按变体显式拉取；manifest 的字段以 `scripts/make-package.py`（写出方）为准。
- **交付物二已完成（R6）**：`scripts/fetch-models.cmake`（**非 Python**，落实你 R4 的自定义要求：clone 后用内置脚本从 release 下真实件）。默认只列不拉 ✓；SHA512 取自 release 的 `manifest.json`，字段以写出方 `scripts/make-package.py:404-413` 为准（`id`/`file`/`sha512`/`version`/`compatVersion`/`directory`/`size`/`models`，顶层 `bundleVersion`+`packages[]` 按 `id` 排序）；下载后**自行**校验摘要，不符即删除并报出两侧摘要 ✓；release 标签与 `bundleVersion` 交叉核对（标签 = 版本去掉末尾零分量，`docs/Packages.md:28`）✓；已存在且摘要正确的归档直接复用 ✓；`KEEP_ARCHIVES` / `MANIFEST`（离线）✓。**离线自检 29 项全绿**（`.tmp/check-fetch-models.py`，临时工具不入库）：列表、拉取、校验、复用、保留、未知变体、标签不符、空清单、坏 JSON、坏摘要、离线十一条路径。
- **未联网验证（按你的选择）**：自检全程用本地伪造 release（`file://`）。附带实测：脚本曾成功取回**真实** release `models-v0.1` 的 `manifest.json` 并列出 4 个包、`bundleVersion 0.1.0.0`（即 schema 与标签规则与真实发布一致），但**四个包本体从未下载过**。
- **发现·未解（留档待查）**：cmake 4.3.1 下 **untyped `-DOTTER_FETCH_OUTPUT=…` 会被丢弃**（同一名字带 `:PATH=` 在最小探针里可见、在真脚本里又不可见；`-DFOO`/`-DAB_CD`/长随机名一律正常；环境变量每次都可靠）。**未定位根因**，故脚本改为「环境优先、cache 变量其次」，文档与示例统一用环境变量；脚本注释与 README 均已写明该实测，避免用户把它误当成用法错误。

### 3.1 候选 A（未采用）：构建期生成，沿用模型夹具先例

- 新增 `scripts/make-test-packages.py`：表 + `write_tree(root)` + `compare(root)` + `main()`（`--output` / `--check DIR` / `--list`）。
- `src/tests/auto/Analysis/CMakeLists.txt` 里按模型夹具的写法加 `add_custom_command`：产物 `${CMAKE_BINARY_DIR}/test-packages/.stamp`，命令为「跑生成器 + 打戳」；`test_AnalysisLoad` / `test_AnalysisRuntime` 依赖它，宏改为构建目录；无 Python 时两条测试 `DISABLED`（D-OTP2）。
- 改动面：新增 1 个脚本 + 1 个脚本自测；改 2 处（`CMakeLists.txt` 的生成块与宏、`test_AnalysisLoad.cpp:31-33` 的 `packages()`，`test_AnalysisRuntime.cpp:61` 不用改如果宏名保留）；删 28 个跟踪文件；改 2 处文档。
- 优点：与本仓既有惯例一致；CI 零改动（Python 已必备）；本机开发者不会因为忘记跑脚本而跳过这两条测试；注册数不变。
- 代价：构建里多一次 Python 调用（纯标准库、13.9 KB 输入、毫秒级）。

### 3.2 B：与 wolf 同口径

- 生成器同上，但**不进构建**：新增缓存变量 `OTTER_TEST_PACKAGES_SOURCE`（默认空），测试经环境变量读取，缺失时 `skip(77)`；CI 里加一步生成并把目录传进去。
- 优点：与 wolf 完全同口径，两个仓的开发流程一致；构建对 Python 无新增依赖。
- 代价：本机必须记得先跑脚本，否则两条装载测试静默变成跳过（虽有 77，但 ctest 只显示 Skip）；CI 需要新增生成步骤与参数（wolf 已这么做，有先例）。

### 3.3 C：测试内用 C++ 直接构造

- 把 13 个包的内容变成 C++ 原始字符串，由共享 helper 在临时目录里落盘（与 `test_AnalysisLoad.cpp:53-66` 现有写法合并），删除 28 个文件与宏。
- 优点：不引入脚本、不改构建、不改 CI，无数据缺失概念，改动面最小。
- 代价：13.9 KB 声明变成 C++ 字面量，可读性与 diff 友好度下降；包内容无法被 lint 工具直接消费；与 wolf 的做法分叉。

## 4. 实施顺序（选定 A 后；B/C 同理调整）

1. 快照与基线：记录 `git ls-tree -r HEAD -- src/tests/auto/Analysis/packages` 的 28 个 blob 哈希与 sha256 清单（`.tmp/otter-packages-baseline.txt`）。
2. 写生成器（表逐包记录 id/witness，不推导）+ `--check`。
3. 自证：`--check` 在未改动树上 0 差异；生成物与基线逐字节一致；`unittest` 全过。
4. 接线（A：构建期生成）并改两个测试的定位方式。
5. `git rm` 掉 28 个文件（单独一笔提交，可单独回滚）。
6. 门禁：本机全新配置 + 构建 + ctest 全绿；确认注册数仍为 12、无新增跳过。
7. 文档：更新 §2.6 指出的两处；把本方案的「已完成」记录写回本文档。
8. 清理 `.tmp` 下本轮产物；提交按主题分离（脚本 / 接线 / 删除 / 文档）。

## 5. 风险

1. **CRLF 陷阱**：等价比对必须走对象库字节（§2.1），否则会在工作区一侧误判差异。生成器写 LF。
2. **包 id 例外**：§1.4 的三个 id 若被"推导"出来，装载测试会因 id 不符而失败——表里逐包写死，并有自测守护。
3. **静默缩水**：形态 A 若把两条测试改成 `DISABLED` 而不加守卫，CI 会静默少测（本仓已有针对模型测试的同类守卫可仿制）。D-OTP2 的推荐即为此。
4. **宏名保留与否**：若改宏名（如 `OTTER_TEST_PACKAGES_DIR`），`test_AnalysisRuntime.cpp` 也要改 1 处；保留原宏名则只需改 CMakeLists 的定义。
5. 强依赖 `.stamp` 语义：`.stamp` 必须最后打（生成器成功才打），否则中断的半成品会被当成完整目录（模型夹具的注释已强调这一点，照抄）。

## 6. 不做的事

- 不动 `scripts/make-package.py`、`scripts/check-declarations.py`、`scripts/make-model-fixtures.py` 及其自测——它们是发布链路与既有机制，本方案只新增同类物件。
- 不动 CI 的既有守卫与步骤（形态 A 下完全不需要改 CI）。
- 不把发布声明目录（仓根 `packages/`，已移出跟踪）牵扯进来。
- 不引入第三方依赖；生成器只用标准库。
- 不碰他人未跟踪的 `docs/plans/asr-integration.md`。

## 7. 决策台账

| 编号 | 决策 | 结论 | 来源 |
| :-- | :-- | :-- | :-- |
| D-OTP1 | 测试包去向形态 | **已定**：外部目录 + 缺数据跳过 77，用 CMake 脚本重建 | R4 交互确认 |
| D-OTP2 | 新增机制是否引入 Python | **已定**：不引入（改用 CMake 脚本） | R4 交互确认 |
| D-OTP3 | 验收是否含跳过路径 | **已定**：纳入 | R4 交互确认 |
| D-OTP4 | 等价判据用对象库字节而非工作区 | 已定（沿用 wolf 阶段纪律） | R2（wolf 阶段）；R4 复核 |
| D-OTP5 | 生成器不自造包 id，表内逐包记录 | 已定（§1.4 三个例外） | 本轮（R4）探索 |
| D-OTP6 | 模型夹具是否改从 release 下载 | **已定**：新增 CMake 下载脚本、按 manifest 校验 SHA512；现合成机制不动 | R4 交互确认 |
| D-OTP7 | CI 保障（生成步骤 + 守卫） | 已定：实施时一并落地 | R4 建议项 |
