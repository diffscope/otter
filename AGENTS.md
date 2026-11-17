# 与本仓库协作的约定

otter 是建立在 synthrt 之上的分析契约库，开发约定与 synthrt 相同。下面各节先列共通的约定，再列本仓库特有的部分。

## 提交

- **提交信息不带任何 AI 署名**，不写 `Co-Authored-By`，不写 `Generated with`。
- **只写一行**，使用英文、首字母大写的祈使句和美式拼写，不写正文，也不加 `fix(scope):` 这类前缀。设计理由写在代码注释和 `docs/` 中，不要在提交信息中长篇叙述。较早的提交使用 Conventional Commits 前缀，不改写历史。
- **一个提交一件事**。同一个文件承载两批改动时，用 `git show HEAD:<path>` 取出旧内容、只叠加其中一批、提交、再放回完整版本，不要图省事整文件暂存。
- 每个提交自身必须能构建、能通过测试，拆分出来的中间状态也一样。

## 注释与文档

- **LLVM 风格**：`///` 写在声明上方。**从不用 `\brief`**，视为浪费空间。
- class 的 private 成员对象与成员函数注释，以及 `.cpp` 中实现细节的注释，使用普通 `//` 双斜杠注释，不使用 `///`。文件是否为私有头文件不影响这一规则。
- 用 Doxygen 命令：`\c` 标识符、`\a` 参数、`\note`、`\warning`、`<tt>` 多词片段。**不要用反引号或引号**。
- **注释里不要破折号，也不要用分号连接从句。** 该断句就断句。
- 注释用美式拼写。
- **不写考古式注释**。解释「为什么现在必须这么写」的留下，叙述「以前是什么样、后来修了」的删掉，那是 git 的职责。
- 不擅自重命名既有标签或措辞。
- Markdown **段落不要硬折行**。一段就是一行，让编辑器去换行。代码块和表格不受此限。
- Markdown 文件**结尾不留多余换行**。
- 引用上位规范写作 `spec 2.4 §<章节名>`，**不写行号**。规范原文在 synthrt 仓库的 [`docs/ds-spec-2.4.md`](https://github.com/diffscope/synthrt/blob/main/docs/ds-spec-2.4.md)，本仓库不保存副本。
- 设计决策登记在 [`docs/otter-design.md`](docs/otter-design.md) 的决策台账，以 A 编号引用。其余章节只陈述结论，依据写在台账里。

## 验证

- **看到测试通过之前，先确认 build 的退出码是 0**。构建失败时 ctest 跑的是上一轮的旧二进制，会给出虚假的绿色。
- **批量改动后核对「改了几处」，而不是「能不能编译」**。语法合法不等于语义正确：`grep -o <pattern> | wc -l` 数的是实际次数，`grep -c` 数的是行数。
- 新增测试后确认断言真的执行了，空 suite 也会「通过」。用 `--report_level=short` 看断言数。
- 分清断言的是当前行为还是设计意图。测试可能只是把缺陷固化了下来。
- 依赖数据的测试有两种不运行的方式，都不会让 ctest 失败：配置时找不到带 `onnx` 与 `numpy` 的 Python，四个模型测试注册为 disabled；运行时缺少夹具、驱动或 ONNX Runtime，测试以跳过状态退出。**ctest 的退出码在这两种情况下都是 0**，要看末尾列出的未运行项。测试包由 `scripts/make-test-packages.cmake` 生成，经 `OTTER_TEST_PACKAGES_SOURCE` 传入。

## 改代码

- C++ 的目录、命名、格式、注释和头文件引用遵守 synthrt 的 [`docs/Development.md`](https://github.com/diffscope/synthrt/blob/main/docs/Development.md)。已知的不一致：本仓库现有头文件的保护名没有带模块目录，新文件按该文件的规则写。
- `.gitattributes` 规定所有文本文件以 LF 入库、以 LF 检出，声明文件与 schema 的字节参与包的哈希，所以行尾在任何平台上都不能变。
- **`sed` 的替换表达式必须带行号地址**，否则是全局替换，且后续表达式会匹配前面已改过的文本、层层嵌套。
- 改完跑 `clang-format`（仓库有 `.clang-format`，ColumnLimit 100），**只对自己创建或修改的文件跑**。范围从 `git diff --name-only` 与自己新建的文件得来，不要取 git 的未跟踪文件列表，其中可能有不属于本仓库的数据。
- 不要 `sed -i` 扫全目录。
- 依赖由 `scripts/vcpkg-workspace.cmake` 按 `scripts/vcpkg-sources.json` 安装到 `deps/`，用法见 [README](README.md)。插件、测试与插件目录的 CMake 函数来自 synthrt 安装的 `SYNTHRT_MODULES_DIR`，不在本仓库重复实现。

## 判断与沟通

- **断言之前先验证**。「公开 API 长这样」不等于「它真的能这么用」。
- 用户的质疑基本都是对的，**先重新验证，不要辩护**。
- 发现自己错了就直接更正，并把结论写回对应的文档条目，注明原判断错在哪。
- 不确定就说不确定，不要用推测填补。

## 文档去处

| 内容 | 位置 |
|---|---|
| 值得长期保留的经验、设计记录 | `docs/claude/`（codex 写 `docs/codex/`） |
| 问题清单、交接、临时分析 | `.cache/claude/`（已 gitignore） |
| 设计决策 | `docs/otter-design.md` 的决策台账 |
| 面向用户的说明 | `README.md`、`docs/` |
| 实施计划 | `docs/plans/` |
