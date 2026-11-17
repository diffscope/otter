# TIFA 接入 otter：Align 变体、`otter/tifa` 包与迁移记录（V1）

> **本文记方案期事实**：文中的状态、版本与读数是方案制定与实施当时的快照；**现行状态见 [`docs/otter-design.md`](../otter-design.md)**。

> **本文档性质**：本地预开发的方案与实施记录（滚动追加）。**远端 otter 正在重构**，本轮的改动与结论只落在本地分支 `analysis-level-1`，不推送、不发布；迁移者按 §4 的变体形状与 §10 的实施记录接手即可。 **状态**：V1 方案完成；**D1–D4 与 D7–D10 已获用户确认**（结论见 §1 表末与 §9）。**P0–P4 与 P7 已完成并实测通过**； P5（诊断指标）与 P6（完整 G2P）按 D2/D3 缓办（与契约相关的部分不混进主干，见 §9 分期表）。 **门禁最终读数**（当时快照）：宿主调用形态（逐 take）下 **42/42、逐边界零差异**；batch-8 口径同时报出，成因见 §10.8.1。 `ctest` 实测 **10/10**（含 `test_Tifa`）。 **2026-09-30 迁移（历史）**：四个仓的远端分支被**强制重写**，本地分支已不在远端历史内；otter 侧当时把本地 tifa 提交 rebase 到重写后的新头 `f4820d9`（保持单分支）；此后本地历史又**从根重写为 4 个提交**，与 `f4820d9` 无共同祖先。那次重写做了什么、为什么、旧号去哪找，见 **§11 的历史索引附录**。 **本文不含本机绝对路径、用户名与机器名**；模型与素材一律用 `<...>` 占位或仓内相对路径（`docs/plans/hfa-align.md` 早期版本曾在 oracle 与素材说明里残留本机路径，现已改为中性描述）。

**范围**：给 otter 增加第四个出厂变体 `tifa`（TIFA 强制对齐器）：把 TIFA 的推理链路（**五张现成 ONNX 图 + 宿主侧解码/打分**）移植成 otter 的 inference 模块，并配套声明、打包、测试与实测比对。

**锚点**（引用行号前请按快照自行复核）：

| 树 | 观察点 | 引用写法 |
| :-- | :-- | :-- |
| otter | 本仓 `analysis-level-1`；方案期的锚点 `f4820d9`（2026-09-30 重写后的远端头）**已不再是基线**——本地历史此后**从根重写为 4 个提交**（tip `bc36a95`），与它**没有共同祖先**，所以 `git rev-list --count f4820d9..HEAD`、`git diff f4820d9..HEAD` 之类的对账都作废。旧提交号与备份分支的去向见 **§11 的历史索引附录** | `src/...:行号` |
| TIFA | 上游仓 `openvpi/TIFA` @ tag `v1.0.0`（commit `32a0a13`） | `TIFA@v1.0.0:infer.py:90` |
| TIFA 权重 | release 资产 `TIFA-1.0-ST.zip`，sha256 `6da6832cd2cb981aae1ccc2d9330f8f7c085ce7977d1eb13bff63531d038a88b`（实测复核一致） | §10.1 |
| OTTER 设计 | `docs/otter-design.md` 的 A 系台账与未决 | `otter-design.md:A27` |
| wolf | 同层兄弟仓，**scheme 命名与语义的唯一来源**（`pinyin`/`jyutping`/`romaji`/`arpabet`） | `wolf:docs/linguist-distribution.md:72-75` |

---

## 0. 结论先行

1. **形态天然适配。** TIFA 的发布物里没有端到端推理脚本：`ONNX.md` 规定五个模块图（`spectrogram` / `model` / `prepare` / `score` / `select`）由宿主拼装，宿主还要实现候选网格、整词候选 DP、Viterbi 解码、时间轴与文本层（`TIFA@v1.0.0:ONNX.md:5-34`）。这正是 otter「抽参器只跑模型、宿主备音频、otter 不做解码」的定位（`otter-design.md:A24`）。
2. **上游只发 `model.pt`，ONNX 需自行导出，且已实测可行。** release 资产只含 `model.pt` 与配置/词典/G2P 资产；用上游 `deploy.py` 导出成功（约 15 s），五图签名与 `ONNX.md` 一致（§2.3、§10.1）。因此**不需要**在 otter 里维护导出器，只在记录里写清导出命令、参数与产物指纹（D6）。
3. **契约面不需要新增 Level。** Align L1 的 `languages`（ISO 639-3 + scheme + lyrics + phonemes）、`defaultLanguage`、 `silenceLabel`、`knobs` 足以装下 TIFA 的**对齐输出**（`AlignApiL1.h:99-154`）。但 TIFA 的三处功能装不进现有结果类型： **发音书写层**（TextGrid 的 `words` tier）、**发音候选与分数**、**四个诊断指标**（§3.3）；这三处按 D2 决定「扩契约 / 分期内做 / 只记录」。
4. **主干是「一个变体插件 + 五会话 + 宿主算法」。** 新目录 `src/plugins/inferenceinterpreters/tifa/`、声明按 `packages/tifa/` 的形状（**声明不入库**，装配时由 `make-package.py --declarations <声明目录>` 指向，一变体一子目录）；`check-declarations.py` 与 `make-model-fixtures.py` 两处**当前写死 `hfa`**，不参数化则新变体被**静默跳过全部 align 校验**（`scripts/check-declarations.py:510-513`）。
（**时效注 2026-10-03**：`check-declarations.py` 已参数化——`MODEL_KEYS` 现含 `(ALIGN, "tifa")`（`:105`、`:134`、`:150`），
ALIGN 的语言表文案也按 variant 取名（`:553`）；`make-model-fixtures.py` 的变体是否已参数化以该脚本现文为准。
上面写死 `hfa` 与所引 `:510-513` 是方案制定时的现状。）
5. **验证有现成 oracle。** 同一份权重、同一段音频，`TIFA@v1.0.0:infer.py` 产的 TextGrid 即参考输出；判据沿用 hfa 先例：**标签逐条相同、起止偏差 ≤ 1 帧（10 ms）**（`docs/plans/hfa-align.md:83`、`build/hfa-run/compare.py:52-53`）。

### 0.1 本地预开发与远端重构的关系（迁移者先读）

| 项 | 现状 | 对本次移植的含义 |
| :-- | :-- | :-- |
| 远端 otter | 正在重构（用户口径；本仓无「预开发/远端」章节） | 本地只提交、不推送；**改动尽量落在「新增变体」这一维度**，避免动 `Align` 契约与库内共用代码 |
| 契约（`include/otter/Api/Align/1/`） | 已由 A19/A27 定稿，读取函数对一切变体共用（`AlignApiL1.h:156-159`） | 变体只实现 `run()`；**若确需扩契约（D2），单列一期并与远端同步**，不混进变体提交 |
| lint / 夹具脚本 | 变体表写死 `rmvpe`/`game`/`hfa` | 本次必须参数化（§5.2），否则校验静默失效 |
| 包版本 | 四个变体的声明**当前都是 `0.1.0.0`**（布局 `inferences/*/inference.json`，A26 后），由 release `models-v0.1` 提供（四包 + 4 项 manifest）；**声明不入库**，装配时由 `make-package.py --declarations <声明目录>` 指向打包机上的目录（一变体一子目录） | `tifa` 声明同版 `0.1.0.0` |
| 发布会话 | 未发令 | **本轮不打包上传、不建 release**；只做本地装配与 manifest 验证 |

---

## 1. 需人工确认的关键点

| 编号 | 决策点 | 推荐 | 备选与取舍 |
| :-- | :-- | :-- | :-- |
| **D1** | 「TIFA 全部功能」的边界与分期 | **按 §3.4 的 P1–P6 分期，全部功能都做，但契约相关的部分单独成期**（③诊断/④G2P 完整版不与主干混提交） | 一次性全做：单批过大、与远端重构相撞；只做对齐主干、其余不做：与用户"全部功能"口径不符 |
| **D2** | 诊断指标（agreement/confidence/determinacy/monotonicity）与「发音候选+分数」的**契约承载** | **先不动 Align L1**：主干期把它们算在变体内部、作为**可选出口**（先只写进记录与未决），**待远端重构落地后再提 A 系条目扩契约** | ①本轮直接扩 Align L1（`Result` 加 `diagnostics`、`PhoneInfo/WordInfo` 加候选与分数）：同步面大（头文件、读取器、`docs/schemas`、lint、测试、远端合并冲突）；②永不外露：等于砍掉 TIFA 的两个卖点 |
| **D3** | 歌词形式与 G2P 归属 | **`lyrics="scheme"`**：宿主给 scheme 写法（拼音/粤拼/romaji/arpabet），变体自带 **「书写单位→音素」四本词典**（release 内随包，共约 3.4 MB）；**不移植** cpp-pinyin / MeCab / LSTM 三套 text→scheme 转换器 | ①变体自带完整 G2P：要到 MeCab+UniDic 与 LSTM ONNX beam search，依赖与工作量最大，且与 wolf 的分工重复；②只声明 cmn：覆盖面倒退 |
| **D4** | 实测通道与素材 | **以「真实素材 + TIFA Python oracle 逐音素比对」为门禁**；ds-editor-lite 无头合成作为**可选**补充（当前合成声库不在本机，且 lite 尚无 Align 通道，`docs/lite-integration.md`） | 只用合成音频：需先解决声库与 lite 侧新接口，链路长；只用真实素材：覆盖面受素材限制 |
| **D5** | 模型与图产物是否入库 / 是否发布 | **（历史）当时的决定：声明入库、权重不入库**（当时做法：`packages/.gitignore` 只忽略 `*.onnx`，见 §5.3），本轮只本地装配、不发布。**现行口径：声明亦不入库**——包（声明 + 模型）随模型发布，装配读 `make-package.py --declarations <声明目录>`（一变体一子目录）；现行版本 `0.1.0.0`、发布标签 `models-v0.1`（四包 + 4 项 manifest） | 入库权重：仓库膨胀（五图约 160 MB）；发布：远端重构期不做 |
| **D6** | ONNX 导出器的归属 | **用上游 `deploy.py` 导出**，otter 只记录命令、参数与产物 sha256 | 在 `otter/scripts/` 维护导出脚本：把上游模型代码搬进 otter，多一份维护面与依赖 |
| **D7** | 变体/包命名与版本 | `variant="tifa"`、包 `id="otter/tifa"`；**（历史）当时定 `version=compatVersion=0.2.0.0`**（与当时三包同版），**现行四包统一 `0.1.0.0`、随 release `models-v0.1` 发布** | 与现有包不同版：manifest 组装复杂化 |

**确认结果**（选项式提问，本会话第 1 轮，逐条由用户选定）：

| 编号 | 选定 |
| :-- | :-- |
| D1 + D2 | **分期全做，主干期不动契约**：先落地对齐主干（P1–P4，含影响对齐选择的发音打分），诊断指标与候选报告先记录为未决，待远端重构落地后再提契约扩展（P5）。 |
| D3 | **`lyrics="scheme"` + 变体自带四本「书写单位→音素」词典**；不移植 cpp-pinyin / MeCab / LSTM 三套文字→书写单位转换器（完整 G2P 列为 P6，按需再启动）。 |
| D4 | **以现有真实素材做门禁**：TIFA Python `infer.py` 的 TextGrid 作 oracle，与变体逐音素比对；ds-editor-lite 无头合成作为可选补充（当前无声库）。 |
| 语言覆盖（新增 D10） | **eng/jpn/yue 也要实测**，不只做 cmn；素材来源见 §8.6（待定，本轮需解决）。 |
| D7/D8/D9（见 §9） | 按推荐执行（`variant="tifa"`、`otter/tifa`（**历史版本号 `0.2.0.0`，现行 `0.1.0.0`**）；扁平 `dictionary<Language>` 键；`silenceLabel="SP"`、`nonSpeechPhonemes` 不声明）。 |

> 其余 D 项（D5 打包与发布、D6 导出器归属）用户未单独选择，按推荐执行且**可回退**（D5：只本地装配、不发布；D6：用上游 `deploy.py` 导出并把指纹写进 §10.1）。

---

## 2. 现状事实（只读探索所得，行号以锚点快照为准）

> §2.2/§2.4 中带 `[子代理]` 的条目来自只读探索子代理的阅读记录，**引用前请回源码复核**；§2.3 与 §10.1 的图签名、字节数与 sha256 为主代理**本机实测**（导出脚本实跑 + `onnx.load` 读取）。

### 2.1 otter 侧

| 事实 | 证据 |
| :-- | :-- |
| Align 是独立契约 `org.openvpi.otter.inference.Align` Level 1；读取函数 `readAlignSchema` 对所有变体共用 | `include/otter/Api/Align/1/AlignApiL1.h:23`、`:26`、`:157-169` |
| 结果类型只有 `language`/`scheme`/`words[{text,start,duration,phones[{text,start,duration}]}]`；结果必须**铺满整段**（未归属的间隔成为 `silenceLabel` 的词） | `AlignApiL1.h:261-271`；`silenceLabel` 为空则「不命名静音、不保证铺满」（`otter-design.md:467`） |
| 声明面：`sampleRate` 必填，`channelCount`/`maxSegmentDuration`/`languages[]`/`defaultLanguage`/`nonSpeechPhonemes`/`defaultNonSpeechPhonemes`/`silenceLabel`/knobs 可选 | `AlignApiL1.h:99-154`、`AlignApiL1.h:164-169`（必填/可选语义）；`src/lib/Api/Align/1/AlignApiL1.cpp:205-207`（knobs 白名单） |
| Align 目前只有三个 knob：`nonSpeechThreshold` / `nonSpeechMinDuration` / `gapFill`（白名单硬编码） | `AlignApiL1.h:145-154`；`scripts/check-declarations.py:190-191` |
| 变体形状（hfa 为唯一先例）：`plugin.json` 的 `interpreters[].variant`、解释器构造、声明的 `variant`、变体 `Triplet` 四处一致；插件 IID 为 `org.openvpi.synthrt.plugin.InferenceInterpreter` | `src/plugins/inferenceinterpreters/hfa/plugin.json:1-9`；`docs/otter-design.md:607-620` |
| ONNX 不直连 onnxruntime：经 dsinfer 驱动 `spec.package().synthUnit().runtimeService(ds::InferenceDriverPlugin::IID, "onnx")` → `createSession()` → `SessionOpenArgs` | `src/plugins/inferenceinterpreters/onnx/OnnxSupport.cpp:33-61`；`otter-design.md:A3` |
| 会话输入输出名在 C++ 侧硬编码；hfa 用 `waveform` 入，`ph_frame_logits`/`ph_edge_logits`/`cvnt_logits` 出 | `src/plugins/inferenceinterpreters/hfa/main.cpp:64-69` |
| 模型自带文件的键名、采样率、帧移**来自模型文件**，声明只做一致性对撞（不一致即拒载） | `src/plugins/inferenceinterpreters/hfa/main.cpp:160-163`（模型 `config.json` 的 `sample_rate`/`hop_size`）、`src/plugins/inferenceinterpreters/hfa/main.cpp:742-833`（加载期对撞） |
| 一次执行一个；取消由 `AnalysisTask` 承载，提供者只在 `run()` 里轮询 `cancelled()`，并在 `stop()`/`waitForFinished()` 里**先调基类** | `AlignApiL1.h:309-320`；`docs/otter-design.md:A15` |
| **lint 对未登记的 `(interface, variant)` 静默降级**：只 warn 后 return，后续 exports/configuration/模型互核**全部不发生** | `scripts/check-declarations.py:510-513` |
| `check_align_declaration` **按契约分派、内容写死 hfa 模型语义**（读 `config.json` 的 `mel_spec_config`、`vocab.json` 的 `vocab`/`silent_phonemes`/`non_lexical_phonemes`/`dictionaries`），错误文案含变体名 | `scripts/check-declarations.py:598-611`、`scripts/check-declarations.py:614-726` `[子代理]` |
| CI **不跑** `packages/` 的声明 lint（声明不入库，CI 没有声明可跑）；全仓唯一真实调用点是打包脚本 | `.github/workflows/ci.yml:102-106`；`scripts/make-package.py:236-251`、`scripts/make-package.py:388` `[子代理]` |
| 夹具由脚本生成真签名假图，CMake 探测到「Python + onnx + numpy」才生成，否则相关用例 `DISABLED`；运行期缺件 `SKIP_RETURN_CODE 77` | `src/tests/auto/Analysis/CMakeLists.txt:76-133` |
| `make-model-fixtures.py` 里 align 夹具的 `"variant"` 由 `align_declaration()` 的 `variant` 参数给出（默认 `"hfa"`，tifa 夹具显式传 `"tifa"`） | `scripts/make-model-fixtures.py:1654-1655`（默认值）、`scripts/make-model-fixtures.py:1701`（hfa 夹具）、`scripts/make-model-fixtures.py:1790`（tifa 夹具）`[子代理]` |
| 三个模型测试注入 `OTTER_TEST_FIXTURE_DIR`/`OTTER_TEST_PLUGIN_DIR`/`OTTER_TEST_DRIVER_PLUGIN_DIR`/`OTTER_TEST_ONNXRUNTIME_DIR` | `src/tests/auto/Analysis/CMakeLists.txt:129-135` |
| 本机已有可用构建树与依赖（Debug 全量测试 exe、`onnxruntime.dll`/`DirectML.dll`、vcpkg 依赖树）；CUDA 关、DirectML 开、测试走 CPU EP | `[子代理]`：`build/agent-tests/`、`scripts/vcpkg-ports/synthrt-main/portfile.cmake:36-50`、`src/tests/auto/Analysis/test_Hfa.cpp:80`。**迁移后已过时**：这类手工指定 include/lib 的树**不能再配置**，改用 README 的 vcpkg manifest 流程（先 `git submodule update --init scripts/vcpkg`，再 `vcpkg install` + `cmake -B <dir> …`） |

### 2.2 TIFA 侧（上游 `v1.0.0`）

| 项 | 事实 | 证据 |
| :-- | :-- | :-- |
| 定位 | Token-Imputing Forced Aligner：音频 + 文本 → 词/音素时间区间；自带发音打分与无参考诊断指标 | `TIFA@v1.0.0:README.md:1`、`:7-13` |
| 发布物 | release 只有 `TIFA-1.0-ST.zip`（一个 PyTorch 检查点 + 配置 + 词典 + 英文 LSTM-G2P 资产），**不含 ONNX** | release 资产清单；§10.1 开箱核对 |
| ONNX 形态 | 五图分工：`spectrogram`（log-mel + maskT）、`model`（相似度 + token logits）、`prepare`（打分模板）、`score`（片段代价）、`select`（按 choices 选路径）；宿主负责候选网格、整词 DP、Viterbi、时间轴与 TextGrid | `TIFA@v1.0.0:ONNX.md:5-34`、`:54-66` |
| 导出器 | `deploy.py -m <model.pt> -o <out>`，opset 默认 18（域 18–20，注释写明 18 支持打分归约与 DirectML）；导出时关 RoPE 缓存、STFT 用实数幅度、`onnxslim`+`checker` | `TIFA@v1.0.0:deploy.py:11-30`；`deployment/exporter.py:18-43`、`:94-119`、`:137-175` |
| 导出器产出的配置 | `config.json` = `samplerate`/`timestep`/`hop_size`/`fft_size`/`win_size`/`num_mels`/`vocab_size` | `deployment/api.py:26-37`；§10.1 |
| 音频前端 | **全部在图内**（`spectrogram.onnx`）：反射填充 `(784,784)` + hann 窗 2048 + hop 480 + 幅度 + `librosa` mel 基（80 bins, fmin 0, fmax 8000）+ `log(clamp(1e-5))`；**无归一化/预加重/dither** | `lib/feature/mel.py:39-81`；`configs/base.yaml:7-16`（`deployment/context.py` 的 export 分支）`[子代理]` |
| 采样率/帧移 | 48 000 Hz、hop 480（10 ms/帧）、win=fft=2048 | 导出后 `config.json`（§10.1）；`TIFA@v1.0.0:configs/base.yaml:8-11` |
| 词表 | `vocabulary.json` = `{"symbols": {"language/phone": id}}`；保留 id 0/1/2 = PAD/MASK/SPACE（**不出现在符号表里**）；实测 220 个符号、219 个不同 id、id 域 3–221；`AP=3/EP=4/GS=5`；`ja/um` 与 `zh/um` 共 id 83 | `TIFA@v1.0.0:ONNX.md:52`；§10.1 实测 |
| 语言前缀 | **剥本次在用语言的模型码前缀**（不是「只剥默认语言」；更正见 §10.8 缺陷 1）；混合语言靠有序候选标签；`global_symbols`/`stop_symbols`/`merged_groups` 在 `g2p` 配置里 | `TIFA@v1.0.0:README.md:154`；`configs/g2p.yaml:46-52` |
| 解码 | 扁平 Viterbi（`decode_alignment_flat`）：token 状态吃 1 帧并加**原始余弦相似度**，gap 状态加 0；**跳帧代价 `skip_penalty`（默认 0.5）**，含序列边界；`groups` 约束只限制 gap 等待；零宽 span 先统一到允许的 gap 位置再按 `--skip-handling` 处理 | `modules/decoding.py:300-421` `[子代理]`；`infer.py:89-92` |
| 打分解码 | 整词候选 DP（前向/后向 + 段尾 SPACE 尾巴），分数 = 归一化后的联合最优值（不是概率）；`--score-unit levenshtein|word|none` | `inference/scoring.py:149-274`；`README.md:160-189` `[子代理]` |
| 诊断指标 | `agreement`（token 分类 softmax 的均值概率）、`confidence`（span 内相似度均值）、`determinacy`（正相似度在近邻 token 上的集中度）、`monotonicity`（证据指向当前或更后 token 的比例）；写 `statistics/{scores,diagnosis}.json` | `README.md:191-206`；`modules/metrics/reference_free.py`、`inference/backend.py:237-239` `[子代理]` |
| TextGrid | 三个 tier：`texts`（语义词）/`words`（发音书写，如拼音/romaji）/`phones`（音素）；共享时间轴，空档为空串；默认省略零宽音素 | `README.md:132-154` |
| 与 G2P 的分层 | 「书写单位 → 音素」是**词典**（`key<TAB>phonemes`，四本：`ds-zh-pinyin-lite` / `japanese_dict_full` / `jyutping_dict` / `ds_cmudict-07b`）；「文字 → 书写单位」才是 cpp-pinyin / MeCab / LSTM | §10.1 词典开箱；`configs/g2p.yaml:7-45` |
| CLI 默认值 | `--skip-handling omit`、`--skip-penalty 0.5`、`--score-unit levenshtein`、`--oov-handling discard`、`--batch-size 8`、`--input-formats wav,flac,opus,mp3,aac,ogg` | `infer.py:51-117` |

### 2.3 五张图的实测签名（本机导出后 `onnx.load` 读取，opset 18）

| 图 | 输入 | 输出 |
| :-- | :-- | :-- |
| `spectrogram.onnx` | `waveform[B,L]:f32`、`duration[B]:f32` | `spectrogram[B,T,80]:f32`、`maskT[B,T]:bool` |
| `model.onnx` | `spectrogram[B,T,80]:f32`、`tokens[B,N]:i64`、`maskT[B,T]:bool`、`maskN[B,N]:bool` | `similarities[B,T,N]:f32`、`logits[B,N,256]:f32` |
| `prepare.onnx` | `paths[B,P,C]:i64`、`words[B,P]:i64`、`candidates[B,W,C]:bool`、`grouped[]:bool`（标量） | `tokens[B,P]:i64`、`segments[B,P]:i64`、`mapping[B,P]:i64` |
| `score.onnx` | `logits[B,P,256]:f32`、`paths[B,P,C]:i64`、`words[B,P]:i64`、`segments[B,P]:i64`、`mapping[B,P]:i64` | `descriptors[B,P1]:i64`、`lengths[B,P1,C]:i64`、`costs[B,P1,C,P1]:f32`、`tails[B,P1,P1]:f32`、`capacity[B,P1]:i64` |
| `select.onnx` | `paths[B,P,C]:i64`、`words[B,P]:i64`、`groups[B,P,C]:i64`、`choices[B,W]:i64` | `best_tokens[B,P]:i64`、`best_words[B,P]:i64`、`best_groups[B,P]:i64`、`maskN[B,P]:bool` |

> **一处已查清的"表里不一"（实测，见 §10.3）**：`score.onnx` 的 `descriptors` **输出声明**只标了两维（`['B','P1']`，onnxslim 按 `dynamic_axes` 重写了形状注解），但图里它是 `Concat(Unsqueeze(owner), Unsqueeze(segment))`，**实际产出是 `[B,P1,2]`**，与 `ONNX.md:105` 及 Python 参考实现（`inference/scoring.py:106-112`）一致。**结论：导出的形状声明不可信 → 载入期不能用它做校验，要在首次 `run` 之后按实际张量断言（§8 第 2 条）。**

### 2.4 关键常量与算法要点（移植必读）

| 项 | 值/要点 | 来源 |
| :-- | :-- | :-- |
| 采样率 / 帧移 | 48 000 Hz / 0.01 s（hop 480，win=fft=2048，mel 80，fmin 0，fmax 8000） | §2.3 实测 `config.json` |
| `maskT` | `t < round(duration/timestep)`，四舍六入五成双（ties-to-even） | `ONNX.md:92` |
| 图内帧数 | `T = floor((L + win - fft)/hop)`（win=fft 时即 `floor(L/hop)`） | `ONNX.md:74` |
| 语言前缀省略 | 剥**本次在用语言**（CLI 的 `-l` 即其默认语言）的模型码前缀；其余语言的前缀保留在标签里。早期转述成「只剥默认语言」是**错的**，更正见 §10.8 缺陷 1 | `README.md:154`；`inference/callbacks.py:158-163` `[子代理]` |
| 静音/非语音 | 模型**没有**自己的静音/呼吸检测头：词表里有 `AP/EP/GS` 三个无前缀标签，但推理时不自动产生（只可能由文本字面带入）；TextGrid 的空档是"没有词"而不是"静音词" | `ONNX.md:52`；§10.1 词表；`README.md:152` |
| 跳帧 | `skip_penalty` 是**原始余弦分**的代价（不是 log 概率），默认 0.5，边界同样计价 | `infer.py:89-92`；`modules/decoding.py:396-401` `[子代理]` |
| 零宽 | `--skip-handling omit`（默认）剔除零宽音素；`preserve` 给零宽串分配 1 ms；`discard` 整条样本丢弃 | `infer.py:83-88`；`inference/callbacks.py:178-217` `[子代理]` |
| 时间基准 | TextGrid 总时长 = `round(T * timestep, 3)`（帧数×10 ms，**不是音频真实时长**），且若末音素越界则撑到该 offset | `inference/callbacks.py:172-217` `[子代理]` |
| 批量 | 只有当批内有样本存在可打分片段时才调 `model`/`score`；无音频/无 token 的样本整条跳过 | `ONNX.md:64` `[子代理]` |

**词典实测规模（`TIFA-1.0-ST/dictionaries/`，本次清点）**

| 词典 | 键 | 行 | 同键多行（=多候选） | 最长音素数 |
| :-- | --: | --: | --: | --: |
| `ds-zh-pinyin-lite.txt`（cmn） | 615 | 615 | 0 | 2 |
| `jyutping_dict.txt`（yue） | 639 | 639 | 0 | 2 |
| `japanese_dict_full.txt`（jpn） | 177 | 177 | 0 | 2 |
| `ds_cmudict-07b.txt`（eng） | 125027 | 133570 | 7934（6.3%） | 32 |

> 结论：**只有 eng 会产生多候选网格**（cmn/jpn/yue 每个书写单位只有一个候选）；候选顺序 = 文件行序，两侧读的是同一本词典、同一顺序，所以候选编号可以直接对齐。

### 2.5 等价性 oracle 与素材

| 用途 | 内容 |
| :-- | :-- |
| **参考输出（oracle）** | `TIFA@v1.0.0:infer.py <audio> -m <model-dir>/model.pt -l <lang> [--skip-penalty …] [--skip-handling …]` → 同目录 `<name>.TextGrid`（三 tier）；`--stat` 另出 `statistics/{scores,diagnosis}.json` |
| 比对脚本形态 | 判据沿用 hfa 先例「标签逐条相同、起止偏差 ≤ 1 帧（10 ms）」，做法沿用 hfa 的「与参考二进制逐区间对拍」（`docs/plans/hfa-align.md:270`、`docs/plans/hfa-align.md:395-400`）；脚本形态照 hfa 那轮的 scratch 工具（`build/hfa-run/` 的 `runner` + `compare.py`，两侧都不入库），本变体另装一套到 `build/tifa-run/`（§6.1） |
| 素材 | 本机「精标数据集」`fox_data` 各版本（**已实测清点，见下表**）提供**真实** cmn 与 jpn 素材；eng/yue 无真实素材 → 走 §2.6 的合成路线。用户已选定合成路线并指定声库，歌词随机自造。 |

**素材清点（本机实测；只记数据集名与计数，不记绝对路径）**

| 语料 | 规模 | 附带标注 | 用途与实测结论 |
| :-- | :-- | :-- | :-- |
| `fox_v2.1` | 297 条 wav | `.lab`（空格分隔拼音音节）+ TextGrid（`words` + `phones` 两层）+ 84 个 json（含 `raw_text`/`lab_without_tone`） | **cmn 主门禁素材**：`.lab` 正是 TIFA 数据集直接读取的伴生文本格式（`inference/data.py:71-78` 先找 `<stem>.txt` 再找 `.lab`） |
| `fox_v2.2` | 287 条 wav | `transcriptions.csv`：`ph_seq`/`ph_dur`/`ph_num` | cmn 的**音素级时间**参考；实测 `ph_dur` 之和 = 9.37501 s 与 `fox_v2.1` 同 id 的 TextGrid `xmax = 9.37499` 一致（差 2e-5 s，取整），**说明该时间轴就是音频的真实时间轴** |
| `fox_v2` | 213 条 wav | TextGrid | 早期版本，备用 |
| `fox_jp_v1` | 97 条 wav | `transcriptions.csv`：`ph_seq`/`ph_dur`/`ph_num`/`note_seq`/`note_dur` | **jpn 真实素材**（曲目前缀 banira/beat/daze/gatsu/hello/kun）；音素级时间 + 音符数据齐备，**但没有现成的「书写单位」歌词** → 需要从 `ph_seq` 反推 romaji 词（§8.6） |
| `record` | 10 条 wma | 无标注 | 本轮不用 |

> cmn 的 TextGrid/`ph_dur` 属**独立于 TIFA 的人工/精标时间轴**：它不能替代 TIFA oracle 做等价门禁，但可以作为「对齐是否可信」的绝对参考（§6 的第二类读数）。

### 2.6 合成素材的声库（用户指定，本机现有，**不入库、不分发**）

| 项 | 事实 |
| :-- | :-- |
| 声库 | `0913_wolf_club@1.0.0`，`vendor = 夜燐Yarin`，packager 格式（`desc.json` + `characters/` + `inferences/` + `linguists/`），目录内带 `Terms_of_Use_1.2.0.zh-CN.pdf` 与多语言使用说明 → **许可受限，只在本机用于实测，不进仓库、不进包** |
| 语言覆盖 | 依赖 `wolf/lang-cmn` `wolf/lang-eng` `wolf/lang-jpn` `wolf/lang-yue`；四位歌手（`yelin`/`huyinyi`/`shark`/`xuanyi`）；四个书写系统接口齐全：`cmn-pinyin` / `eng-arpabet` / `jpn-romaji` / `yue-jyutping` —— **与 TIFA 的四门语言、四本词典一一对应**，因此同一批歌词可以直接喂给两边 |
| 音素表对撞（本机实测，声学模型 vs TIFA `vocabulary.json`，按 `cmn↔zh`、`eng↔en`、`jpn↔ja`、`yue↔yue` 对齐） | `eng` 42 = 42 **完全相同**；`cmn` 63 ⊆ TIFA 65（TIFA 多 `iai`、`um`）；`jpn` 39 ⊆ TIFA 40（TIFA 多 `um`）；`yue` 73 ⊇ TIFA 70（声库多 `em`/`ep`/`eu`） |
| 差异的两种表达方式 | 共享音素 `um`：声库写作**无前缀**（id 82），TIFA 写作 `ja/um` 与 `zh/um` **共享同一 id 83**（对应 `configs/g2p.yaml` 的 `merged_groups: [[ja/um, zh/um]]`）——语义相同、编码不同 |
| `SP` | 声库把 `SP` 列为无前缀共享符号（id 4）；TIFA 词表里**没有** `SP`。本变体把 `silenceLabel` 声明为 `"SP"` 因此与宿主生态的既有写法一致（§4.4 第 5 条），不是自造新词 |
| 对选素材的约束 | 合成歌词只使用**交集内**的书写单位：cmn 避开 `iai`；yue 避开 `deu`/`lem`/`gep`（§2.7）与会产出 `em/ep/eu` 的韵母；这样两边都能处理，比对才有意义 |
| 驱动方式（实测） | 走 **ds-editor-lite 的无头自动化端点**：一个最小 JSON-RPC over HTTP 客户端拉起无头编辑器（端口写在本机暂存状态文件里；回环地址加入 `NO_PROXY`，避免请求被本机 HTTP 代理截走），逐条提交歌词、收回 wav；**只读地用 lite，不写它的仓库**。驱动脚本与编辑器日志留在本机暂存区（含本机路径，故**不入库**）；迁移后照本节事实重做即可：起无头编辑器 → 用上表的歌手与书写系统接口 → 歌词只取交集 → 收到 wav 后统一成 48 kHz / 16-bit / 单声道 |
| 素材规模（实测） | **真实 11 条**（cmn `buhuji_high_*` 5 条、jpn `banira_*` 6 条，来自 §2.5 的本机精标数据集）+ **合成 31 条**（第一批 `material-{cmn,eng,jpn,yue}` 共 10 条，第二批 `material-syn2` 共 21 条）= **门禁的 42 个 take**（四门语言都有）。喂给两侧的是 `material-48k/<语言>[-<批次>]/` 下的 42 组 16-bit 副本，oracle 目录里另有 53 份 TextGrid（含早期批次），门禁只取与 take 同名的那一份 |

### 2.7 上游数据不自洽（本次实测发现）

| 项 | 事实 |
| :-- | :-- |
| 现象 | `jyutping_dict.txt` 共 639 个键，其中 3 个键的候选含**词表里不存在**的音素，且没有替代写法：`deu → [d eu]`、`lem → [l em]`、`gep → [g ep]`（`eu`/`em`/`ep` 在 `vocabulary.json` 里查不到） |
| 对照 | cmn 词典产出的音素全部可在词表拼出（`iai`/`um` 是「词表有、词典没用到」，无害）；jpn 同理（多 `um`）；eng 的 42 个完全对齐 |
| 声库侧 | 声库（§2.6）的 yue 音素表**有** `em`/`ep`/`eu`——即「声库能唱、词典能产出、TIFA 词表缺失」 |
| 影响 | 变体若把这类音节照常送进图，`Vocabulary::id()` 返回 0（padding），会**静默给出错误对齐**；Python 侧默认 `--oov-handling discard` 则丢掉整条样本 |
| 处置 | 变体按 §4.5 第 7 条处理（丢候选 → 丢词 → 整句无可用词才拒）；选词纪律避开这 3 个音节；**不把 3 个音素补进声明**（声明必须能被词表拼出，lint 也会拦） |

---

## 3. 契约面：复用 Align L1（含承载不下的点）

### 3.1 映射表（TIFA → Align L1）

| TIFA 概念 | otter 落点 | 说明 |
| :-- | :-- | :-- |
| 语义词（TextGrid `texts` tier） | `WordInfo.text` | 调用方给的词（scheme 写法），逐词原样回写 |
| 发音组（TextGrid `words` tier） | **无落点** | L1 的词只有一个文本字段；见 §3.3 |
| 音素（`phones` tier） | `WordInfo.phones[]` | 按语义词聚合：一个词的所有发音组的音素按序拼接 |
| 空档（无词区间） | `silenceLabel` 的插入词 | L1 要求结果铺满整段（现以 `silenceLabel` 非空为条件，见 `docs/otter-design.md:450-452`）；TIFA 的 gap 状态不产标签，由变体补标签 |
| `AP/EP/GS` | 声明的 `phonemes`（无前缀标签按语言无关处理）或 `nonSpeechPhonemes` | TIFA **不自行检测**它们 → 见 §4.4 差异登记 |
| 语言/写法 | `language`（ISO 639-3）+ `scheme`（wolf 命名）+ `lyrics="scheme"` | cmn/pinyin、yue/jyutping、jpn/romaji、eng/arpabet |
| `skip_penalty` / `skip-handling` / `score_unit` | **knob 面装不下**（三个 knob 白名单硬编码） | 见 §3.3 与 D2 |

### 3.2 声明草案（声明目录里 `tifa/inferences/align/inference.json` 的 exports 部分）

```
sampleRate 48000 · channelCount 1 · maxSegmentDuration <待定，见 D-风险 §8.1>
languages:
  cmn/pinyin     lyrics=scheme  phonemes=<词表中 zh 前缀集合去前缀；四个集合的计数见 §10.1>
  yue/jyutping   lyrics=scheme  phonemes=<yue 前缀集合去前缀>
  jpn/romaji     lyrics=scheme  phonemes=<ja 前缀集合去前缀>
  eng/arpabet    lyrics=scheme  phonemes=<en 前缀集合去前缀>
defaultLanguage cmn
silenceLabel    "SP"        （TIFA 词表里没有 SP 这个符号，它是本变体给"空档词"的拼写）
nonSpeechPhonemes  <空>     （模型不自带非语音检测 → 如实声明"不检测"）
knobs           <按 §3.3 决定>
```

音素表必须与 `vocabulary.json` 里带该语言前缀的集合**逐一相等**（多一个少一个都拒载，沿用 hfa 的加载期互检）。

### 3.3 装不进的四处（D2 的输入）

| # | 装不下的东西 | 现状依据 | 三个候选处置 |
| :-- | :-- | :-- | :-- |
| a | **发音书写层**（TextGrid `words` tier） | `WordInfo` 只有 `text`/`start`/`duration`/`phones` | ①接受丢失（宿主/变体各自 G2P 可复现）；②扩 `WordInfo` 增 `script`；③扩成"词→发音组"两层 |
| b | **发音候选与分数**（`scores.json`） | 同上 | ①只在变体内部使用（选择结果影响对齐，这已足够）；②扩契约回传候选与分数 |
| c | **四个诊断指标** | `Result` 只有 `language`/`scheme`/`words`；`docs/otter-design.md` §11 未决 · 「`Align` 的置信度」已明确「Level 1 不含置信度，凭空加字段等于猜语义」 | ①先记录、后扩；②本轮就扩（`Result` 加可选 `diagnostics`） |
| d | **解码/打分 knob**（`skipPenalty`/`skipHandling`/`scoreUnit`） | knobs 白名单硬编码三键（`AlignApiL1.h:145-154`；`scripts/check-declarations.py:190-191`） | ①不暴露（写死默认：`skipPenalty=0.5`、`skipHandling=omit`、`scoreUnit=levenshtein`，与 Python 默认一致）；②扩契约暴露 |

> **推荐**：a/b/d 走 ①，c 走「①先记录、后扩」，即**主干期不动契约**（D2）。

### 3.4 功能→分期对应（D1）

| 期 | 内容 | 对应 TIFA 功能 |
| :-- | :-- | :-- |
| P1 | 声明与包形状 + lint 参数化 | 底座 |
| P2 | 插件骨架：五会话装配、模型文件读取、加载期互检、声明导出 | 底座 |
| P3 | 宿主算法：候选网格、`prepare/score/select` 驱动与整词 DP、Viterbi、静音补词、时间轴 | 对齐主干 + **发音打分（影响对齐选择）** |
| P4 | 真实权重实测：与 Python TextGrid 逐音素比对 | 等价性门禁 |
| P5 | 诊断指标 + 打分报告出口（按 D2） | 「诊断指标」「发音候选报告」 |
| P6 | G2P 完整版（若 D3 选②：cpp-pinyin/MeCab/LSTM-ONNX/多语言混排/OOV 策略） | 「多语言 flex 配置」「文字输入」 |

---

## 4. 变体 `tifa`

### 4.1 目录与文件

```
src/plugins/inferenceinterpreters/tifa/
├── plugin.json         # name=ottertifa，interpreters[0]={Align, 1, "tifa"}
├── main.cpp            # 声明读取、模型文件读取、加载期互检、会话、run()
├── Grid.{h,cpp}        # 候选网格：scheme 词 → 词典音素 → paths/words/groups/candidates
├── Scoring.{h,cpp}     # prepare/score/select 三图驱动 + 整词候选 DP + 归一化分数
├── Decode.{h,cpp}      # 扁平 Viterbi（含 skip penalty、group 约束、零宽归位、span 提取）
└── Metrics.{h,cpp}     # （P5）四个诊断指标
```

> **迁移修正**：本目录**没有** `CMakeLists.txt` —— 上游改成在 `src/plugins/inferenceinterpreters/CMakeLists.txt` 里用 `otter_add_interpreter_plugin(ottertifa tifa SOURCES… LINKS…)` **集中声明**（连带 IID 与 `plugin.json` 拷贝）。早期草稿写的"照 hfa 建 `project(ottertifa)` + `otter_add_plugin`"在新基线上不成立。

### 4.2 `configuration` 键（变体私有，宿主不读）

键名与逐键路径（五张图、`config`、`vocabulary`、四本 `dictionary*`、`languages` 映射）的**唯一权威**是 `docs/otter-design.md` **§6「模型包结构」**一节的 TIFA 段（`:566-603`，标题见 `:483`）；本文不复制那份清单——同一份键值抄两处必然漂移。本节只记两条与改代码直接相关的约束：

1. **声明的每门语言都必须有对应词典键**，缺一即拒载（§4.3 第 3 条）。
2. 键名与集合需**同时**登记进 `scripts/check-declarations.py` 的三张表（`MODEL_KEYS`/`CONFIGURATION_KEYS`/`LANGUAGE_NUMBERING`），否则该包被静默跳过校验（§2.1）。

### 4.3 加载期互检（任一条不成立即拒载）

1. `config.samplerate` == `exports.sampleRate`（48 000）；`config.num_mels`/`vocab_size` 与图输入输出形状一致（`80`/`256`）。
2. `channelCount` == 1。
3. 每门声明语言：`configuration.languages` 有模型码，且该码对应的 `dictionary*` 键存在、文件在包内且非空。
4. 声明语言的 `phonemes` == `vocabulary.json` 中带该模型码前缀的符号集合（去前缀后逐一相等，打印两侧差集）。
5. `silenceLabel` 非空且**不与任何语言的音素重名**（它是变体自造的拼写，不与词表冲突即可）。
6. `nonSpeechPhonemes` 为空（本变体不检测）；若将来声明，必须 ⊆ `{AP,EP,GS}`。
7. 同一 `(language, scheme)` 只出现一次；`defaultLanguage` ∈ 声明语言。
8. 每门语言的 scheme 必须是 wolf 的既有命名（`pinyin`/`jyutping`/`romaji`/`arpabet`），否则拒载（防止同一记法出现两种拼写）。

### 4.4 一次执行流程（`TifaExecutive::run`）

流程意图：**先校验**（非空 `lyrics`、语言/scheme 在声明内、采样率与 span 长度）→ **音频前端**（库内 `prepareSamples` 之后交 `spectrogram` 图）→ **文本侧**（歌词按空白切词、查词典成候选网格 → `prepare` 出模板 → `model`/`score` 与宿主整词 DP 选候选 → `select` 出 `best_tokens`）→ **相似度**（`model` 第二次进图）→ **帧解码**（`Decode` 的扁平 Viterbi）→ **落到宿主时间轴**（词/音素聚合、剥**本次在用语言**的模型码前缀、空档补 `silenceLabel` 词）；每步之间轮询取消。多候选、空标签与零宽归位的细节见 §4.5 与 §4.7。

> **步骤编号与进度值以代码为准**：`src/plugins/inferenceinterpreters/tifa/main.cpp` 的 `run()` 内有分段注释与 `report()` 调用点，本文不复述。本节早先抄过一份数字，且**漏了其中一档**——这正是「同一读数只允许一处权威」的理由；那次缺陷与修法见 §10.8。

### 4.5 与参考实现的有意差异（逐条登记，落地时写进代码注释）

| # | 差异 | 理由 |
| :-- | :-- | :-- |
| 1 | 歌词只吃 scheme 写法（不跑 cpp-pinyin/MeCab/LSTM） | D3：与 wolf 分工一致；完整版列为 P6 |
| 2 | 结果多出 `silenceLabel` 的插入词，且铺满整段（现以 `silenceLabel` 非空为条件，见 `docs/otter-design.md:450-452`） | L1 的结果语义（`AlignApiL1.h:261-271`）；Python 版只留空档 |
| 3 | 零宽处理固定 `omit`（Python 默认），`preserve/discard` 不暴露 | §3.3d；以 D2 决定是否扩 knob |
| 4 | `skip_penalty` 固定 0.5、`score_unit` 固定 `levenshtein` | 同上（两者都是 Python 默认值） |
| 5 | 不输出 `words` tier（发音书写）与 `scores.json`/`diagnosis.json` | §3.3a/b/c；P5 按 D2 处理 |
| 6 | 单样本执行（L1 一次一个 span），无 batch/多进程 | L1 与 `AnalysisTask` 的既定语义 |
| 7 | 「候选里含词表没有的音素」与「书写单位未收录」同样处理：丢该候选 → 丢该词 → 整句无可用词才拒 | 上游词典与词表不自洽（§2.7）；Python 默认 `--oov-handling discard` 会丢掉整条样本，对宿主过于激进，故取 `force` 那一侧的行为 |
| 8 | 打分模板的**退化表**上不复制参考的失控行为 | 实测（`build/tifa-run/review-probe/scoring_diff.py`：一侧是 `Scoring.cpp` 的逐行转写、一侧直接 import 参考 `inference.scoring._select_sample`）：3000 张随机表**0 选择分歧、0 错误形态差异**；6 类退化表里 4 类不同——参考对"分片只写词号不写段号"与"`capacity` 少写哨兵项"直接抛错（含 `IndexError`），对"词号越界"与"代价越界为 `-inf`"静默取首个候选；移植版前者给出选择、后者按具体理由拒绝。**导出图不会写出这些表**，所以只承诺在真实表上等价 |
| 9 | 时间用**宿主时间轴上的绝对秒**，不是相对这段音频起点的偏移 | L1 的结果是绝对时间（`AlignApiL1.h:261-271`）；参考脚本按整文件推理、只会给相对起点的偏移，宿主一切片就会放错位置（`src/plugins/inferenceinterpreters/tifa/main.cpp:607-608`、`src/plugins/inferenceinterpreters/tifa/main.cpp:1331-1338`） |

### 4.6 P2 实现要点（只读核实所得，快照 `2049afd`；引用前请回源码复核）

1. **骨架与加载**：`plugin.json` 的 `name` 必须等于库文件名，也就是 CMake 工程名 `ottertifa`（bundle 解析器按 `{"","lib"}×{"",".dll",".so",".dylib"}` 在该目录里找）；bundle 目录为 `inferenceinterpreters/tifa`；`interpreters[]` 的三元组不得重复。注册点在 `src/plugins/inferenceinterpreters/CMakeLists.txt:56-90` 的 `OTTER_HAS_DSINFER` 块内（tifa 的注册在 `src/plugins/inferenceinterpreters/CMakeLists.txt:85-89`；**不要**放进 `OTTER_BUILD_TESTS` 块，那是 stub 的位置），新目录的 `CMakeLists.txt` 逐行照 `hfa/CMakeLists.txt:1-30`。（**迁移修正**：这一段已过时——注册点不再是 `add_subdirectory` 里的逐行照抄，而是 `OTTER_HAS_DSINFER` 块内的一句 `otter_add_interpreter_plugin(ottertifa tifa SOURCES… LINKS…)` 集中声明，插件目录里**没有** `CMakeLists.txt`；行号也请以新头快照为准。）
2. **声明的读取时机**：`readAlignSchema` 只强制 `sampleRate`，未知键与未知 knob 键直接拒。**`createExports` 先于 `createConfiguration` 被调用**，所以在 `createExports` 里**不能**读 `spec.configuration()`——要用 `spec.manifestConfiguration()` 自己再读一遍（hfa 即如此）。
3. **模型自带文件的读取**：dsinfer 的公共 API **没有包内文件读取接口**，包内文件由共享的 ONNX 支持层读（`otter::onnx::readTextFile` 里的 `std::ifstream`：`src/plugins/inferenceinterpreters/onnx/OnnxSupport.cpp:89`；JSON 入口 `otter::onnx::readJsonObject` 在同文件 `:102-104`），基准是 `spec.declarationPath().parent_path()`（`src/plugins/inferenceinterpreters/hfa/main.cpp:933`、`src/plugins/inferenceinterpreters/tifa/main.cpp:1457`）。tifa 的 `config.json`/`vocabulary.json`/四本词典同法，声明里的相对路径按此基准解析。
4. **五会话**：先例是 game 变体（多模型容器 + 循环开会话 + 失败即整体回滚）。驱动比会话活得久；镜像按 (realpath, useCpu) 引用计数共享。**`SessionOpenArgs2` 在本 dsinfer 版本不存在**，只有 `ds::Api::Onnx::SessionOpenArgs{bool useCpu}`；建议把 `prepare`/`score`/`select` 这三张纯 i64/bool 图设为 `useCpu=true`，避开 DirectML/CUDA 对 i64 算子的不确定性（**推断，未实测**）。
5. **取消与析构**：`stop()`/`waitForFinished()` 必须先调基类，再逐会话 stop/waitForFinished；`OnnxSession::stop()` 在空闲时报错，必须吞掉。
6. **张量**：f32 与 i64 用 `createFromView`，bool 用 `createFromRawView(ITensor::Bool, …)`（1 字节/元素），0 维标量（`grouped`）传空 shape。
7. **加载期互检**：hfa 的九条（采样率对撞、单声道、语言不重复、型号码存在、词典名存在、词典文件存在、音素集合相等、非语音标签已知、静音标签在词表内）在 `hfa/main.cpp:742-833` 各有实现；tifa 的差别见 §4.3 与 §5.2 的清单。
8. **夹具与测试**：`scripts/make-model-fixtures.py` 的 align 分支已把 `variant` 参数化（`align_declaration()` 的 `variant`，`scripts/make-model-fixtures.py:1654-1655`），tifa 的五张真签名假图与 tifa 声明模板也已随 P2 落地（五个 `build_tifa_*` 在 `scripts/make-model-fixtures.py:614-1031`，tifa 夹具声明在 `scripts/make-model-fixtures.py:1790`）；夹具缺失时 `test_Tifa` 仍按运行期 `exit(77)` 跳过。

### 4.7 P3 算法移植要点（只读核实 TIFA 源码所得，行号属 `TIFA@v1.0.0`）

1. **候选网格**：`P` 是「来源网格容量」——各词的多序列 Levenshtein 对齐列数之和（**不是音素数**）；`words[p]=w+1`（1 基，0 = padding）；`paths/words/groups` 的列 `c` 就是第 `c` 个候选；`candidates` 是前缀真值掩码；`groups` 是候选内 1 基组号，**同组内禁止留空**。（`g2p/encoding.py:49-156`）
2. **五图顺序**：`spectrogram` →（`prepare` → `model` → `score` → 宿主整词 DP → `choices`）→ `select` → `model`（喂 `best_tokens`）→ 宿主 Viterbi。`prepare` 的标量 `grouped=false` 等价于 `--score-unit levenshtein`（CLI 默认）；`none` 时不跑评分三图，`choices` 取每个词第一个候选。
3. **整词 DP**：状态 =（当前段, 该段已用槽位）；跨段要结算上一段的 `tails`（剩余槽位全读 SPACE 的对数概率和）；总目标是**联合最优值**（`max` 递推，不是概率），最后 `/capacity.sum() + log(256)` 归一化；**平局必须按（源状态 rank, 候选号）确定性打破**，否则与 Python 结果不同。（`inference/scoring.py:95-273`）
4. **Viterbi**：帧得分用**原始余弦相似度**，gap 状态 +0；跳过扣 `skip_penalty=0.5`（初始化的跳前缀与每帧 `G_i→G_{i+1}` 各扣一次，序列尾部结束不扣）；`groups` **只限制 gap 等待**，不限制零时间转移；零宽 span 先按「极大零宽 run + 第一个允许的 gap 位置」归位到左右边界。（`modules/decoding.py:272-421`）
5. **零宽三态**：`omit`（默认，剔除零宽音素）、`preserve`（全量改写为整数毫秒、每段至少 1 ms）、`discard`（整条样本不产出）——本变体固定 `omit`（§4.5 第 3 条）。
6. **输出**：时间基准是 `round(T*timestep, 3)`，即**帧数×10 ms 而非音频真实时长**；剥**本次在用语言**的模型码前缀（不是「只剥默认语言」——转述更正见 §10.8 的缺陷 1）；空档不产词 → 本变体按 L1 的语义补 `SP` 词。（`inference/callbacks.py:118-254`）
7. **数值坑**：mel 基是 librosa 的 Slaney 公式（已固化学在图内，宿主无需复刻）；帧数与毫秒取整均为**四舍六入五取偶**（C++ 用 `std::nearbyint`，不要用 `std::round`）；`costs` 的非法槽位是 `-inf`；归一化的运算顺序不可调换。（`lib/feature/mel.py:36-37`、`inference/callbacks.py:173-176`、`inference/scoring.py:136,266-268`）
8. **全局符号/停用符号在推理路径根本没有传入**（只有训练/建词表路径传），因此运行时**不会**按 `SP/sil/pau` 过滤音素；而 `configs/g2p.yaml` 里的 `global_symbols/stop_symbols/merged_groups` 会被误读成推理期行为 → 移植时不要顺手塞进去。（`inference/data.py:90-93`、`g2p/encoding.py:54-71`）

#### 4.7.1 打分路径的补充事实（本轮源码级复核，行号属 `TIFA@v1.0.0` = `32a0a13`）

| # | 事实 | 证据 | 为什么要写下来 |
| :-- | :-- | :-- | :-- |
| a | **`choices` 是 1 基**：候选 id `1..C`，`0` = 「该词未读/无候选」；图内把 `choices` 前面补一列 0，再用 `words` 当行下标 gather，所以 `words=0` 的行取到 0 | `lib/path_traversal.py:5-6,49-51,56-63`；`inference/scoring.py:256` | 本方案早期草稿写成 0 基，已实测纠正（夹具与插件现按 1 基实现） |
| b | **`prepare` 不算任何编辑距离**：`unit="levenshtein"` 只是「按分歧行打 MASK」这一策略的名字；真正的多序列列对齐在**宿主**（列向 consensus 作参照、两侧同时展开、回溯偏好 match > delete > insert，gap 进张量写 0） | `inference/scoring.py:27-82`（无 DP）；`g2p/encoding.py:128-137`；`lib/levenshtein.py:88-125` | 以为 `prepare` 会做对齐会漏掉整段宿主职责 |
| c | **分歧的参照列是第 0 列**（`paths[..., :1]`），不是 consensus、也不是最长候选 | `inference/scoring.py:52` | `ONNX.md:97` 不足以推出这一条 |
| d | 保留常量：`PAD=0`、`MASK_TOKEN=1`、`SPACE_TOKEN=2`，真实音素 token ≥ 3 | `lib/vocabulary.py:6-10`；`g2p/encoding.py:96-97` | 判定「这一行有没有音素」要用 0/1 的语义，不能只看非零 |
| e | **`score` 吃的是原始 `logits`**（图内做 `log_softmax(-1)`），`similarities` 只进对齐 Viterbi，不进打分 | `deployment/exporter.py:83`、`ONNX.md:16-17,96` | 宿主预先 softmax 会双重归一化 |
| f | **输入不变式**：`words=0` 的 padding 行、「无候选词」的行，其 `paths` 必须全 0；否则 `prepare` 的 `shared` 会退化成真，把第 0 列 token 当真实音素写进模板 | `inference/scoring.py:52,73` | 违反时**静默**错对齐，必须在宿主侧校验 |
| g | `choices` 初值：代码是 `candidates.argmax(-1) + 1`（首个 True 的 1 基下标），文档 `ONNX.md:64` 写的是 `any(candidates, -1)`；**在前缀打包不变式下二者恒等** | `lib/path_traversal.py:15-18` vs `ONNX.md:64` | 移植按代码实现，并自己强制「候选列前缀打包」 |
| h | `skip_penalty` **不在整词 DP 里**（`inference/scoring.py` 全仓命中 0 次），只在帧×token 的 Viterbi 里 | `modules/decoding.py:306,313,332-335` | 别把它塞进候选选择 |

---

## 5. 包与打包

### 5.1 包形状（**声明不入库**，声明与权重都在装配时才进打包目录）

```
packages/tifa/                       # 形状约定；声明不入库，声明目录下一变体一子目录
├── desc.json                        # id=otter/tifa, version=compatVersion=0.1.0.0, runtimeLevel=1
└── inferences/align/inference.json  # interface/level/variant/name/exports/configuration（§3.2 + §4.2）
```

**装配后的包形状与逐键路径见 `docs/otter-design.md` §6「模型包结构」的 TIFA 段**（唯一权威， `:566-603`）：打包时才装配五张 `.onnx`、`config.json`、`vocabulary.json` 与四本词典；模型文件与**声明本身都不入库**（声明从 `--declarations` 指定的目录读取，见 D5）。

### 5.2 必须同步的脚本改动（否则静默失效）

| 文件 | 改什么 | 不改的后果 |
| :-- | :-- | :-- |
| `scripts/check-declarations.py` | ①`MODEL_KEYS` 增 `(ALIGN,"tifa")`；②`CONFIGURATION_KEYS` 增白名单；③`LANGUAGE_NUMBERING` 增 `"string"`；④`check_align_declaration` 按变体分派（tifa 的模型文件语义与 hfa 不同），去掉写死的 "hfa variant" 文案 | 该包**全部 align 校验被跳过**（`scripts/check-declarations.py:510-513`） |
| `scripts/test_check_declarations.py` | 增 tifa 的正/反例（新变体、缺词典、音素表多/少、错采样率） | lint 变更无门禁 |
| `scripts/make-model-fixtures.py` | 新增 `build_tifa()`（五张真签名假图）+ tifa 声明模板；把 `align_declaration()` 的 `variant` 参数化；坏包矩阵同 hfa 形状 | `test_Tifa` 无夹具 → 运行期 `exit(77)` 静默跳过 |
| `src/tests/auto/Analysis/CMakeLists.txt` | 加 `test_Tifa` 到 `_otter_tests` 与模型测试 `foreach` 列表 | 用例与注入宏不生效 |
| `src/plugins/inferenceinterpreters/CMakeLists.txt` | **迁移后**：在 `OTTER_HAS_DSINFER` 块内用 `otter_add_interpreter_plugin(ottertifa tifa SOURCES… LINKS…)` 集中声明（不再是 `add_subdirectory(tifa)`，tifa 目录也不再自带 `CMakeLists.txt`） | 插件不构建 |
| `packages/.gitignore`（**历史**，当时随仓库跟踪，现已不需要） | 若把模型自带文件复制进 `packages/tifa/` 再入库会被 git 收进去 → 补规则（或改为只在本地产物目录装配） | 160 MB 权重入库 |
| `scripts/make-package.py` | **实测无需改动**（§10.6）：它按声明的后缀收集文件并保持相对路径，tifa 一趟跑通 | — |
| `packages/README.md`（**历史**）、`README.md` | 变体表与计数、打包示例 | 文档漂移 |
| `docs/otter-design.md` | §1 契约表变体列、§4.3 分层树、§7 插件形状、§9 新增 A 系条目、§10 里程碑、§11 未决 | 迁移者看不到入口 |

### 5.3 不做的事（本轮）

不推送、不发布、不建 release；不改 `include/otter/Api/Align/1/*`（除非 D2 选扩契约）；不动 lite（`docs/lite-integration.md` 的 Align 范围说明另议）。

---

## 6. 验证方案（门禁，实施后逐条报数）

| 层 | 手段 | 通过口径 |
| :-- | :-- | :-- |
| 声明与包 | `python scripts/check-declarations.py <声明目录>/tifa`（**必须真的跑到 align 专项**，即 `MODEL_KEYS` 已登记；本机声明目录就是 `packages/`） | errors = 0；故意破坏（缺词典/音素表差一）时能被拦下 |
| 夹具单测 | `test_Tifa`（新写）：声明导出面、happy path 时间线、knob 生效、坏声明矩阵、会话取消 | 全绿；夹具缺失时是 `SKIP(77)` 而非"通过" |
| lint 自测 | `python -m unittest discover -s scripts -p "test_*.py"` | 全绿（含新加的 tifa 用例） |
| 构建门禁 | 本地构建树 configure + build + `ctest` | 全绿；新目标被 `add_subdirectory` 收进 |
| **数值等价（核心门禁）** | 同一权重、同一音频、同一歌词：`TIFA@v1.0.0:infer.py` 的 TextGrid vs 本变体结果 | **标签逐条相同、起止偏差 ≤ 1 帧（10 ms）**；同素材重复跑同一实现结果一致（工具自证） |
| **语言覆盖** | 四门语言各至少一条素材：cmn 用 `fox_v2.1`（真实），jpn 用 `fox_jp_v1`（真实），eng/yue 用 §2.6 的合成素材 | 四门都要在报告里出现读数；缺哪门就写明「缺失及原因」，不用别的语言的结论代替 |
| **绝对参考（附加读数，非门禁）** | cmn 的 `fox_v2.2` `ph_dur` 时间轴 / `fox_v2.1` TextGrid，jpn 的 `fox_jp_v1` `ph_dur` | 只报「偏差分布」，用于判断对齐是否可信；**不作为通过条件**（这些精标时间轴是另一套流程的产物） |
| 打包 | 本地装配 + `manifest.json` 逐条校验（size/sha512 回读） | 与 hfa 同口径；**不上传** |
| 反调参声明 | 若某语言/某素材不达标，**先记录读数再讨论**，不做"把阈值调到看起来对齐"的补偿 | 记录在 §10 |

### 6.1 数值等价门禁的工具链（P4 用）

otter 是库，仓内**没有任何 `int main`**（只有插件与测试），所以"跑变体"这一步由测试可执行文件承担：

| 步 | 工具 | 说明 |
| :-- | :-- | :-- |
| 1 | TIFA oracle | `python infer.py <素材目录或单个 wav> --model <TIFA-1.0-ST>/model.pt -l <语言> -o <oracle 目录>`；伴生歌词读 `<stem>.txt`，没有则读 `<stem>.lab`（`inference/data.py:71-78`）；输出**三 tier** TextGrid：`texts`（词级）、`words`、`phones`（音素级；空档写 `""`，**本次在用语言的前缀已剥**）。实测样例：`buhuji_high_001` → `texts`/`words` 各 23 条、`phones` 43 条，`xmax = 9.38` |
| 2 | scratch runner（照 hfa 先例 `build/hfa-run/runner.cpp`） | 编到 `build/tifa-run/runner.exe`：加载**真实包**（本机声明目录的 `tifa/` + 真 ONNX + 真词典），走 Align L1 跑一条素材，把结果拍平成 `音素:起点:时长` 的 TSV（或同结构 TextGrid）。**scratch，不进仓库** |
| 3 | scratch 比对脚本（照 `build/hfa-run/compare.py:17-53`） | 把 oracle 的 TextGrid 与 runner 的输出都拍平成音素区间序列，报「条数、标签是否逐条相同、起止最大偏差」，退出码表达结论。**scratch，不进仓库** |
| 4 | 工具自证 | 比对脚本先在**同一份输出的两份副本**上跑出 0 差异，再对同一份输入做一次"人为挪 2 帧"的变异必须变红；两侧实现各自重复跑两遍结果须一致（`tool-metric-selfcheck`） |

> 本变体的 L1 结果用 `silenceLabel = "SP"`，而 oracle 的 TextGrid 把空档写成 `""`：比对脚本按已知映射把 `SP` 归一成 `""` 再比，**不把差异算成标签不符**（这是两条契约的写法差异，不是缺陷）。

> **语言标签有两套拼写，都是事实而非笔误**：Align L1 的 `languages[].language` 用 `cmn`/`yue`/`jpn`/`eng` ——与既有先例一致（当时先例：本机声明目录的 `hfa/inferences/align/inference.json` 的 `cmn/pinyin, eng/arpabet, jpn/romaji`， `defaultLanguage: cmn`）；模型自己的词表前缀与 TIFA 的 G2P 用 `zh`/`en`/`ja`/`yue` （`configs/g2p.yaml:9-43` 的 converter `language:`、`inference/data.py` 的 `--language`）。两者由 `configuration.languages` 映射（**逐项取值见 §4.2 指向的设计文档一节**，本文不复制），C++ 侧按 hfa 的做法消费这张表（`src/plugins/inferenceinterpreters/hfa/main.cpp:487-527`）；`dictionary<Language>` 的键名与 `dictionary*` 的**键**用 L1 那一套（`Cmn`/`Yue`/`Jpn`/`Eng`）。

---

## 7. 实施分期（每期独立提交、可回滚）

| 期 | 内容 | 交付物 | 状态 |
| :-- | :-- | :-- | :-- |
| P0 | 事实核对：release 开箱、ONNX 导出、签名与指纹 | 本文 §10.1 | **完成** |
| P1 | 声明与包 + 三处脚本参数化 | `packages/tifa/`、`check-declarations.py`、`test_check_declarations.py`、`packages/README.md`（包内的**声明与 `packages/README.md` 属历史**：现已不入库，声明从 `--declarations` 目录读取） | **完成**（`f841875`） |
| P2 | 插件骨架（会话、模型文件、互检、声明）+ 单候选端到端链路 | `src/plugins/inferenceinterpreters/tifa/`、夹具五图与坏包矩阵、`test_Tifa` 用例 | **完成**（`436708b`、`66f1eb8`；§10.7） |
| P3 | 打分路径（候选网格、`prepare`/`score`、整词 DP、1 基 `choices`） | `Grid.{h,cpp}` 扩展 + `Scoring.{h,cpp}` + 用例 | **完成**（配置合批 + 帧解码按参考重写；空标签与多读音选择都已修，`test_Tifa` 覆盖；见 §10.3–§10.7） |
| P4 | 真实权重实测与数值比对 | 门禁工具（scratch）+ 读数（§10.8） | **完成**：读数以**逐 take 口径**为主（42/42、逐边界零差异），batch-8 口径同时报出（口径与成因见 §10.8.1） |
| P5 | 诊断指标与报告出口（按 D2） | `Metrics.{h,cpp}`（+ 契约扩展若 D2 选②） | **未做**（D2 已定本轮缓办；L1 契约没有该输出通道） |
| P6 | G2P 完整版（若 D3 选②/③） | text→scheme 转换器 | **未做**（D3 已定只支持 `lyrics="scheme"`） |
| P7 | 文档与迁移收尾 | `otter-design.md` A 系与未决、README、`packages/README.md` | **完成**（设计文档台账 A27–A29、包 README、本文 §10 的实施记录与 §11 的历史索引附录） |

**构建纪律**：构建/测试命令按 `README.md:106-118` 的相对路径写法（vcpkg manifest、`build/<dir>`），**不写本机绝对路径**；每期先跑门禁再提交。 **迁移后补充**：流程仍是这一套，但**多了一步前置**——先 `git submodule update --init scripts/vcpkg`（overlay 必须先 checkout），再 `vcpkg install` + `cmake -B <dir> …`；旧的手工指定 include/lib 的构建树已**不能配置**。

> **提交号说明（2026-09-30 迁移后）**：上表与 §10.7 引用的旧提交号（`f841875`、`436708b`、`66f1eb8` 等）属 **rebase 前**的历史，新历史与当前分支上都不存在——**所有 `safety/*` 安全分支（含 `safety/otter-20261001-1754`、`safety/otter-presquash`）都已删除**，这些号只在 `git reflog` 里，随 reflog 过期或 `git gc` 失效（§11 附录）。本地历史此后从根重写为 4 个提交，`f4820d9` 不再是基线。

---

## 8. 风险与未决

1. **`maxSegmentDuration` 取多少**（未定）。TIFA 是全长注意力，帧数二次复杂度；hfa 用 60 s。→ 需实测（P3/P4）后在声明与文档里定值，并记录依据。
2. **导出图的形状声明不可信**（已实测，§10.3）：`score.onnx` 把 `descriptors` 的声明形状写成 `[B,P1]`，实际产出 `[B,P1,2]`。→ 载入期不做形状对撞，改成**首次 `run` 后断言实际张量**；另外 `dsinfer` 的公共 API **没有任何图内省**（无法枚举输入输出名与 dtype），名字只能像 hfa/game 一样硬编码成常量。
3. **[推断] 推理路径不传 `global_symbols`/`stop_symbols`**：`inference/data.py` 调 `encode_paths` 时未传这两个参数（训练路径传了），可能导致 `AP/SP` 等字面标签在推理下的行为与训练不一致。→ 实现前用一次 Python 运行钉死（P3 前置）。
4. **`prepare.onnx` 的 `grouped` 是标量入参**，而 CLI 没有对应开关（`--score-unit` 走 Python 侧模板选择）：本变体按 `grouped=false`（levenshtein 模板，等于 CLI 默认）实现，`word` 模板暂不暴露。
5. **词表里没有 `SP`**：`silenceLabel` 用 `"SP"` 是**变体拼写**而非模型符号；若宿主下游按"标签必须来自模型词表"假设处理会不一致 → 在 §4.4 与声明注释里写明。
6. **素材**：cmn（`fox_v2.1` 297 条，直接带 `.lab`）与 jpn（`fox_jp_v1` 97 条，带音素级时间）都有**真实**素材； eng/yue 走合成（§2.6 声库），选词只取两边词表的交集。剩下的风险是 **jpn 没有现成的「书写单位」歌词**： `fox_jp_v1` 只给 `ph_seq`，需要反推 romaji 词（用 TIFA 自己的 `japanese_dict_full.txt` 反查，或按 `ph_num` 切分音节）。**等价门禁只需要两边喂同一份歌词**，所以反推结果的质量只影响「对齐是否可信」的读数，不影响门禁有效性 → P4 记录反推手段与命中率。**素材与声库均不得入库、不得随包分发**（§2.6 的声库带 Terms_of_Use，`fox_data` 属本机精标数据），仓库里只记数据集/声库名、版本与计数。
7. **与远端重构的合并风险**：变体插件是新增目录，冲突面小；但 P1/P2 会碰 `check-declarations.py`、`make-model-fixtures.py`、测试 CMakeLists —— 迁移时按 §5.2 表逐项核对。
8. **未决（承接 `docs/otter-design.md` §11 未决 · 「`Align` 的置信度」）**：Align L1 的置信度/诊断字段，本轮不扩；P5 若扩，需同时给出 `docs/schemas` 与 lint 的同步面。

---

## 9. 决策台账（本方案编号，落地后并入 `docs/otter-design.md` 的 A 系）

| 编号 | 决策 | 依据 / 来源 | 状态 |
| :-- | :-- | :-- | :-- |
| D1 | 「全部功能」分期（P1–P6），契约相关部分单独成期 | §3.4；用户原始请求「支持 tifa 的全部功能」 | **已确认**（第 1 轮） |
| D2 | 主干期不动 Align L1；诊断指标/候选报告先记录、后扩 | §3.3；`docs/otter-design.md` §11 未决 · 「`Align` 的置信度」 | **已确认**（第 1 轮） |
| D3 | `lyrics="scheme"` + 变体自带四本「书写单位→音素」词典；不移植 cpp-pinyin/MeCab/LSTM | §2.2 G2P 分层；`wolf:docs/linguist-distribution.md:72-75`（romaji/cl 命名一致） | **已确认**（第 1 轮） |
| D4 | 以真实素材 + Python oracle 逐音素比对为门禁；合成通道可选 | `docs/plans/hfa-align.md:83`（先例）；D 子代理实测「本机无声库、lite 无 Align 通道」 | **已确认**（第 1 轮） |
| D5 | **（历史）声明入库、权重不入库**、本轮不发布；**现行：声明亦不入库**，装配读 `make-package.py --declarations <声明目录>`（见上文决策表 D5） | 历史依据：`packages/.gitignore:10`；用户口径「远端重构中，本地预开发」 | 按推荐执行（可回退） |
| D6 | 用上游 `deploy.py` 导出 ONNX，otter 只记录 | §10.1 实测（约 15 s，五图签名符合 `ONNX.md`） | 按推荐执行（可回退） |
| D7 | `variant="tifa"`、`otter/tifa`；**（历史）`0.2.0.0`，现行 `0.1.0.0`** | 当时先例（本机声明目录）：`packages/hfa/desc.json:2-14` | 按推荐执行 |
| D8 | `configuration` 用扁平 `dictionary<Lang>` 键（lint 只对字符串路径做校验） | `scripts/check-declarations.py:531-544` | 按推荐执行（P1 已落地） |
| D9 | 静音补词用 `silenceLabel="SP"`，`nonSpeechPhonemes` 不声明 | §2.4「模型无静音/非语音检测头」；`AlignApiL1.h:137-143` | 按推荐执行（P1 已落地） |
| D10 | 四门语言（cmn/yue/jpn/eng）都要实测，不只做 cmn | 用户第 1 轮的选定；素材来源见 §8.6 | **已确认**，素材待定 |

**确认轮次**：本会话提交方案时 D1–D9 一次性列出（选项式提问）；用户确认后在此行记录"逐条选定/范围确认/实施方式确认"的落点。

---

## 10. 实施记录（滚动追加，事实均实测）

### 10.1 P0 开箱核对与 ONNX 导出（本机实测通过）

| 项 | 结果 |
| :-- | :-- |
| 上游 | `openvpi/TIFA` @ `v1.0.0`（`32a0a13`） |
| release 资产 | `TIFA-1.0-ST.zip`，150 789 526 B，sha256 `6da6832cd2cb981aae1ccc2d9330f8f7c085ce7977d1eb13bff63531d038a88b`（**与 release 页公布值逐字符一致**） |
| 解包内容 | `model.pt`（158 588 551 B）、`config.yaml`（1 567 B）、`vocabulary.json`（4 115 B）、`dictionaries/`（`ds_cmudict-07b.txt` 3 370 331 B / `ds-zh-pinyin-lite.txt` 6 494 B / `jyutping_dict.txt` 6 897 B / `japanese_dict_full.txt` 1 552 B）、`assets/LstmG2p-Eng/`（`encoder.onnx` 2 536 734 B、`decoder.onnx` 1 491 741 B、`char.json`、`phonemes.json`、`config.json`）；**无 `LICENSE`、无 ONNX 推理图** |
| 模型配置（`config.yaml` 实测键值） | `model.arch=ForcedAlignmentModel`、`max_vocab_size=256`、`in_dim=80`、`embedding_dim=256`、`out_dim=256`；`inference.features.audio_sample_rate=48000`、`hop_size=480`、`fft_size=2048`、`win_size=2048`、`spectrogram={type: mel, num_bins: 80, fmin: 0.0, fmax: 8000.0}`；`inference.g2p` 为五转换器链（chinese-pinyin/japanese-mecab/yue-jyutping/lstm(en)/dictionary(zh,ja,yue)/passthrough），词典与资产均以 `@` 前缀相对模型目录引用 |
| 导出 | `python deploy.py -m <model-dir>/model.pt -o <out>`（上游 `deploy.py:11-30`，opset 默认 18）→ **成功，约 15 s**（日志：五图逐个 `Exporting`，`Deployment completed`） |
| 产物 | `spectrogram.onnx` 339 493 B、`model.onnx` 159 792 782 B、`prepare.onnx` 30 776 B、`score.onnx` 23 980 B、`select.onnx` 13 463 B、`config.json` 165 B、`vocabulary.json` 4 115 B |
| 产物指纹（sha256） | `config.json` `3380e63d13754364808ca92c9090e6a489b733864a99f07188ed6e5f457b2549`；`model.onnx` `bc58d7fe8c5fd5f41582e6ef06dd801049df1c0097c198f7d5289f78539f9dcd`；`prepare.onnx` `f55667ed220ba3208d1b121bfc228cd04ef2d10c4e49eabcee72c1f7e08af7ed`；`score.onnx` `2133cb89066e46a2ca1b78f303d11c45ec2670e346b63689639ecc2d39833e97`；`select.onnx` `abfab83229bf3a8e188c137553ed04edefcc59500435e4aba7e6ddc77f71d9d5`；`spectrogram.onnx` `2fe85f8507d1acd998fa854869fda75f3a2fa1d316f1e56ef4ea29df8b2a51a4`；`vocabulary.json` `644c85ac28cfe4f26fcf60de5a6d1fe192c11a6a837746119ef2dbc6dac9246c` |
| 导出环境 | 独立 venv（`--system-site-packages`，复用已有的 torch 2.8.0+cu128 / onnx 1.22.0 / onnxslim），按上游 `requirements.txt` 补齐其余依赖；**未改动上游仓工作树**（`PYTHONDONTWRITEBYTECODE=1`，`git status` 干净） |
| 词表实测 | 220 个符号 / 219 个不同 id（id 3–221）；保留 id 0/1/2 不在符号表；`AP=3`、`EP=4`、`GS=5`；`ja/um` 与 `zh/um` 共享 id 83；按前缀计数：`zh` 65、`yue` 70、`ja` 40、`en` 42、无前缀 3 |
| 结论 | **五图接口与 `ONNX.md` 相符（除 `descriptors` 秩一处，见 §8.2）**；ONNX 路线可行，无需在 otter 内维护导出器（D6 得证） |

### 10.2 P1 声明与 lint 参数化（已完成，提交 `feat(packages): declare the tifa align package`）

| 项 | 结果 |
| :-- | :-- |
| 新增声明（**历史：当时随仓库跟踪，现已不入库**） | `packages/tifa/desc.json`、`packages/tifa/inferences/align/inference.json`：四门语言（cmn/pinyin、yue/jyutping、jpn/romaji、eng/arpabet，全部 `lyrics="scheme"`）、`defaultLanguage=cmn`、`silenceLabel="SP"`、`sampleRate=48000`、`channelCount=1`、`maxSegmentDuration=60`（**待 P3/P4 复核**）、`configuration` 十一处文件加 `languages`（`cmn→zh`、`yue→yue`、`jpn→ja`、`eng→en`）；音素表直接取自 §10.1 的 `vocabulary.json`（四门语言各 65/70/40/42 个，另加三个无前缀共享符号） |
| lint 参数化 | `scripts/check-declarations.py`：登记 `(ALIGN,"tifa")` 的 `MODEL_KEYS`/`CONFIGURATION_KEYS`/`LANGUAGE_NUMBERING`；`check_align_declaration` 改为按变体分派（hfa 保持原路径，改名 `check_hfa_align_declaration`），新增 `TIFA_DICTIONARIES` 表把语言接到词典键 |
| tifa 专项校验 | 采样率对 `config.json` 的 `samplerate`；声明语言必须有对应 `dictionary<Language>` 键且文件在包内；`phonemes` 与 `vocabulary.json` 里带该语言前缀的集合互核（无前缀的共享符号允许出现在任何语言里）；`silenceLabel` 不得与任何声明的音素重名；`nonSpeechPhonemes` 必须是无前缀符号；声明了 `dictionary*` 却没有对应语言 → 警告 |
| 自测 | `scripts/test_check_declarations.py` 增 4 组用例（tifa 好包 0 error/0 warning、采样率不符、缺词典与多词典、音素多一个与少一个、静音标签撞音素）；`python -m unittest discover -s scripts` 20 个用例全绿（迁移后为 25 个；此后随仓库重写与用例增加，**现行整套为 49 个用例**） |
| 真包对撞 | 用 §10.1 的真实导出装配一份包（真实 `config.json`/`vocabulary.json`/四本词典，五张图用占位文件）→ `0 error(s), 0 warning(s)`；再做四处变异（采样率改 44100、cmn 少列一个音素、eng 多列一个音素、删掉 `dictionaryYue`）→ **4 条 error 全部命中**，证明该校验确实在跑，而不是像未登记变体那样被静默跳过 |
| 文档 | `packages/README.md`（四变体表、tifa 装配命令与专项校验说明）、`README.md`（采样率一栏补 48 kHz）、`packages/.gitignore`（补 `config.json`/`vocabulary.json`/`dictionaries/` 三条）——**（历史）后两者现已不入库** |
| 未做 | 插件与夹具（P2）、宿主算法（P3）、真实权重实测（P4）。`scripts/make-package.py` 经核对**无需改动**：它按文件名平铺查找模型文件、再落到声明写的位置，`(directory/inside).parent.mkdir(parents=True)` 已经覆盖 `dictionaries/` 这类子目录 |

### 10.3 导出的图与 Python 参考实现对齐核查（工具自证，已完成）

| 项 | 结果 |
| :-- | :-- |
| 手段 | 用 `onnx.reference.ReferenceEvaluator` 直接跑导出的 `score.onnx`，输入取导出器自带的示例（`B=2, T=32, N=6, W=2`，`paths/words/candidates` 同 `deployment/exporter.py:129-131`），与 Python 的 `prepare_scoring` + `score_fragments` 逐张量比对 |
| 结果 | **五个输出的形状与样本值完全一致**：`descriptors[2,7,2]`、`lengths[2,7,2]`、`costs[2,7,2,7]`、`tails[2,7,7]`、`capacity[2,7]`（`costs` 的 `-inf` 位置也一致） |
| 由此确认 | `score.onnx` 的输出**声明**秩 2 是注解错误（onnxslim 按 `dynamic_axes` 重写了形状注解），实际张量秩 3，与 `ONNX.md:105` 和 `inference/scoring.py:106-112` 相符 |
| 更正 | 本文档早前记录的「实测 `[B,P1]`、与 `ONNX.md` 不一致」是**误读形状声明得出的错误结论**，已在 §2.3、§8.2 更正。教训：读 ONNX 元数据不等于读运行时张量，校验以运行时为准（与 `tool-metric-selfcheck` 的纪律一致） |
| 附带收获 | 这段比对脚本本身就是 P4 之前最有用的「图语义」验证器：换任意随机输入即可继续压测五张图，不必等 C++ 侧写好 |

### 10.4 素材路线与声库对撞（已完成，只读）

| 项 | 结果 |
| :-- | :-- |
| 声库 | 用户指定 `0913_wolf_club@1.0.0`（packager 格式，含 `cmn-pinyin`/`eng-arpabet`/`jpn-romaji`/`yue-jyutping` 四个语言接口与四个歌手），许可受限 → 只在本机使用 |
| 对撞 | 把声库 `inferences/acoustic/*.phonemes.json` 与 TIFA `vocabulary.json` 按 `cmn↔zh`、`eng↔en`、`jpn↔ja`、`yue↔yue` 逐语言比对：`eng` 两边 42 个**完全一致**；`cmn` 63 ⊆ TIFA 65（多 `iai`、`um`）；`jpn` 39 ⊆ TIFA 40（多 `um`）；`yue` 声库 73 ⊇ TIFA 70（多 `em/ep/eu`） |
| 结论 | 共享音素 `um` 在声库侧是**无前缀**符号、在 TIFA 侧是 `ja/um`≡`zh/um` 的**合并 id**（语义相同）；`SP` 在声库侧是真实共享符号、在 TIFA 侧只作为本变体的静音拼写。**选词只取交集** → 写进 §2.6 与 §8.6 |

### 10.5 P4 前置：素材与 oracle 的自证（已完成，读数为实测）

| 项 | 读数 |
| :-- | :-- |
| oracle 可用性 | `infer.py`（`-l zh` / `-l ja`）在 48 kHz 素材上跑通：cmn 5 条、jpn 6 条全部 `SUCCESS`，各出三 tier TextGrid（`texts`/`words`/`phones`） |
| 素材配平 | 原素材是 44.1 kHz/24-bit 单声道；统一转成 **48 kHz/16-bit 单声道**后再喂两侧，**两条链路都不再重采样**（转换是 scratch 命令，不入库） |
| jpn 歌词反推 | 用 `japanese_dict_full.txt` 反查 `ph_seq`：6/6 成功；自证方式是**把反推词再展开回音素序列，要求与原标注逐条相等**（脚本 scratch，不入库） |
| 标注时间轴可信度 | `fox_jp_v1` 的 `ph_dur` 之和与 wav 时长**逐条完全相等**（6/6，差 0.0000 s）→ 标注时间轴就是音频时间轴 |
| 绝对参考读数（**非门禁**） | jpn：phone 条数与标签与精标**完全一致**，最大边界差 26.2 ms / 32.5 ms；cmn：条数一致，少量韵母写法不同（`uo`↔`o`、`ui`↔`ei`、`uan`↔`an`），一条最大边界差 19.7 ms，另一条因开头长静音被 TIFA 判得比精标短而**整条平移约 0.30 s** |
| 结论 | oracle 与素材两条前置均自证通过。第二条也说明了为什么门禁必须是「oracle vs 变体」：精标时间轴来自另一套流程，与 TIFA 在静音处理上本就不同 |

### 10.6 打包路径清点（P1 补充证据，已完成）

| 项 | 结果 |
| :-- | :-- |
| 结论 | **`scripts/make-package.py` 无需任何改动**即可装 tifa：`--variant tifa --models <导出的 7 个文件 + 4 本词典>` 一次跑通 |
| 原因 | 脚本按声明里 `MODEL_KEYS` 登记为该变体模型文件的配置键收集文件（`scripts/make-package.py:104-129`），并按**声明内的相对路径**保持目录结构（`scripts/make-package.py:126-128`），所以 `../../<图>.onnx` 落在包根、`../../dictionaries/*.txt` 落在包子的 `dictionaries/` ✓ |
| 装配实测 | 包目录 `otter-tifa/`：五张图 + `config.json` + `vocabulary.json` + `dictionaries/`（4 本）+ `inferences/align/inference.json` + `desc.json`；内置 lint 通过；产出确定性 zip `otter-tifa-0.2.0.0.zip`（144 MB；**历史**：当时的包版本号，现行四包均 `0.1.0.0`）与逐文件 sha512 片段 |
| 未做 | 没有并入发布用 `manifest.json`（不属本轮范围，也没给定 bundleVersion）；产物全在 build 目录，**未提交** |

### 10.7 P2 插件骨架与单候选链路（已完成，提交 `test(fixtures): …` + `feat(tifa): …`）

| 项 | 结果 |
| :-- | :-- |
| 提交 | `436708b`（夹具：五图 + 好包 + 6 个坏包 + 声明模板）、`66f1eb8`（插件 `tifa/`、`test_Tifa.cpp`、两处 CMake 注册） |
| 夹具自证 | 脚本 `_check_signature` 逐图断言名字/元素类型/秩/动态轴 + `opset=18`；每张图与脚本内的 numpy 参照实现逐张量比对（`checked 20 tensors of 5 graphs`）；另用外部脚本 + `onnxruntime` 复核（tol 1e-5）；旧夹具（hfa/rmvpe/note）逐文件 SHA256 **0 changed / 0 missing** |
| 反例矩阵 | `fixture-align-tifa-{wrong-rate,no-dictionary,missing-dictionary,wrong-phonemes,unknown-silence,extra-dictionary}`：命名沿用**先变体后缺陷**（与 `test_Tifa.cpp` 的消费方一致）；`wrong-rate` 取 16000 以便与提供者的报错文案对撞；`extra-dictionary` 必须仍能加载（0 error / 1 warning） |
| 测试 | `ctest --test-dir build/agent-tests` → **9/9 通过**（含 `test_Tifa` 7 个用例与既有 5 个模型测试、声明 lint 测试）——**迁移后修正**：`_otter_tests` 是 **9** 个目标 = 5 个非模型 + 4 个模型（`test_Rmvpe`/`test_Game`/`test_Hfa`/`test_Tifa`），原文"既有 5 个模型测试"与该清单不符；新基线上 Debug 树只有 **6 个非模型用例**通过，4 个模型用例因 ONNX Runtime 运行目录未就绪报"找不到指定的模块"（Release 树实测 10/10） |
| 本阶段实现范围 | 五图各一会话；实际调用 `spectrogram` → `select` → `model` → 宿主 flat Viterbi（**单候选捷径**：`choices` 取每词首个可用候选）；`prepare`/`score` 会话已打开但未调用（P3 接上） |
| 落地时修掉的缺陷 | ① `run()` 里 `features->heard` 被 `std::move` 后仍被 `runModel` 读 → 空 vector `.back()`，异常被 `catch(...)` 吞成无后缀消息；② `choices` 语义按上游纠正为 **1 基**（见 §4.7.1a）；③ 测试里 `1.005 * 48000` 截断取整少 1 个样本 → 改 `std::llround` |
| 有意差异的落地 | §4.5 第 7 条（未收录音素 = 未收录书写单位）已实现并带端到端用例（复制夹具包到临时目录、只改副本词典追加 `zq → qq`，验证「单说被拒」「与真词同句时只保留真词且不冒充静音」） |

### 10.8 真实权重下的首次等价读数（门禁已跑通，工具自证通过）

工具：`build/tifa-run/{runner.cpp,build.ps1,compare.py,gate.ps1}`（scratch，**不入库**）。
- `build.ps1` 从 `build/agent-tests/build.ninja` 里读出测试目标自己的 `DEFINES/FLAGS/INCLUDES/LINK_LIBRARIES` 来编译 runner（避免第二份路径清单漂移）。**迁移后过时**：该树已不能配置，门禁工具要改从新构建树的 `build.ninja` 读同一组标志，重跑前先补这一段。
- `compare.py` 的门禁 = 音素标签逐个相等 且 每个边界 ≤ 1 帧（10 ms）；**自证**：把某一侧边界人为移动 2 帧必须被判红，同输入重复跑读数必须逐位一致——两条都在实测中通过。
- 两侧读同一批文件：原始素材与合成素材都先统一转 48 kHz/16-bit 单声道，**两条链路都不再重采样**。

| 样本 | 读数 |
| :-- | :-- |
| `cmn` 真实 5 条 | 全部 **PASS**：音素数逐个相等（40/39/44/37/50），标签全同，最大边界差 1.00 帧，其中一条 100 个边界**零差异** |
| `cmn` 合成 1 条 | **PASS**：16 音素，32 个边界**零差异** |
| `jpn` 真实 6 条 + 合成 3 条 | **计数全部相等、时间逐个吻合**（例：`ja/a at 0.120` 对 `a at 0.120`），但标签带着 `ja/` 前缀 → 判红（见下方「发现的缺陷 1」） |
| `yue` 合成 3 条 | 同上：26/30/26 音素一一对应、时间吻合，标签带 `yue/` 前缀 |
| `eng` 合成 | 2 条同上带 `en/` 前缀；`syn-eng-002` **计数 44 对 45**（见「发现的缺陷 2」） |
| 词级交叉印证 | `cmn/buhuji_high_001`：变体 23 个词、末词结束 9.375 s；参考 23 个区间、`xmax=9.38` ✓ 总时长一致（帧数×10 ms） |

**发现的缺陷 1（真缺陷，门禁抓到的）：非默认语言的音素前缀没剥。** 变体只剥**声明默认语言**（`cmn`）的前缀，于是 jpn/eng/yue 的结果里带着 `ja/`、`en/`、`yue/` ——而 `exports.languages[].phonemes` 承诺的是**裸音素**，宿主拿到的标签会不在自己的白名单里。根因是方案里「只剥默认语言前缀」这条**转述有误**：它原写在 §2.4 的「语言前缀省略」行与 §4.7 第 6 条（两处已就地更正）。参考实现剥的是**本次在用的语言**（CLI 的 `-l` 就是它的默认语言），故单语言运行时它永远剥掉。修正：剥「本次对齐所用语言」的模型码前缀；cmn 5/5 通过正是因为它恰好等于声明默认语言。

**发现的缺陷 2（预期差异，等 P3）：`syn-eng-002` 音素数 44 对 45。** eng 是唯一有多候选书写单位的语言（§2.4 实测：6.3% 的键有多行），参考实现跑 `prepare→score→整词 DP` 挑候选，而当前变体走**单候选捷径**（取每词首个候选）→ 候选选得不同，音素数自然不同。这正是 P3 要接上的路径；接上后重跑本条即可。

#### 10.8.1 全量语料读数（42 个文件，四门语言，两批合成 + 真实素材）

**读数快照**：`analysis-level-1`（含 `006fd55` 的帧解码重写），**强制重跑** → 本快照是 **batch-8 口径**；**逐 take 口径（宿主调用形态）的读数是 42/42、逐边界零差异**，两种口径的成因见下方的「读数口径」。下表只是该快照的**分组视图**，不作为任何结论的出处。

> **读数口径（2026-09-30 复核）**：本表是**对着 batch-8 形态的 `oracle-48k`** 量出来的，该 oracle **可复现** （与当前 Python 原版逐字节相同）。但参考对**批组成**敏感（同一 take 换批，其相似度 mean|Δ| = 0.049），而宿主调用本变体是**一次一个 span**——所以**逐 take（`--batch-size 1`）**才是"等价移植"的判据。本表的数字要读作"**与 batch-8 oracle 的差异**"，其中的词内音素差异多半由此而来；两种口径都要报，不许只留一侧。

| 语料 | 通过 | 判红 |
| :-- | :-- | :-- |
| cmn 真实 5 + 合成第一批 1 | **6/6** | — |
| cmn 合成第二批 | 3/5 | `syn2-cmn-001`（词内音素：我们 `h iao` 对参考 `h`）、`syn2-cmn-002` |
| jpn 真实 6 | **6/6** | — |
| jpn 合成第一批 | 2/3 | `syn-jpn-001` |
| jpn 合成第二批 | **4/4** | — |
| yue 合成第一批 3 + 第二批 4 | **7/7** | — |
| eng 合成第一批 | 1/3 | `syn-eng-002`（`music` 少词尾 `k`）、`syn-eng-003` |
| eng 合成第二批 | 2/8 | `syn2-eng-001/002/003/005/006/008`（002 少词尾 `k`；006 读 `ae` 对参考 `ax`） |

**三条独立的失败原因（不要混为一谈）**：

| 代号 | 现象 | 归因 | 状态 |
| :-- | :-- | :-- | :-- |
| A | 结果里出现**空字符串音素标签**（`  at 2.940` 对 `ax at 2.940`），其后整条链错位一位 | 打分路径选列后取标签的环节（P3 新增逻辑） | **已修**：标签改取候选元数据（`42c08fe`）。本轮 42 个样本里已无空标签（`classify-failures.py` 输出全部为真实音素） |
| B | `syn2-cmn-001` 音素数 27 对 26（cmn 词条**只有一个候选**，排除候选选择） | 待查：丢词/多词、或同一书写单位两边读音个数不同 | **已实测定位**：词级音素数一致，差异在**词内**——`hiao` 我们 `h iao` 对参考 `h` |
| C | 帧解码终局状态选择与参考不等价（§10.9 C1，已复验） | `Decode.cpp` 未忠实移植参考状态机 | **已按参考重写**（`006fd55`，含取自参考输出的用例）；**实测它只解释 13 个失败里的 2 个**，其余 11 个与之无关 |

**同时确认的事实**：37 份参考输出里**没有一个零宽音素**（`build/tifa-run/count-skipped.py` 实测 0/37），所以 C1 在本语料上没有以"整个音素被吃掉"的形式出现——但它仍可能表现为边界偏移（B/C 之前不做结论）。

### 10.9 只读对抗复核轮（P2/P3 落地后，找反例）

复核子代理只读（未改仓库、未构建、未 git 写），目标是**推翻**已落地的实现。它报了 5 条，我**亲自抽复** 了最重的两条（用它自己的证据不够——它的复现是"把 C++ 逐行转写成 Python"，转写本身可能是错的）：

| # | 断言 | 我的复验结果 | 处置 |
| :-- | :-- | :-- | :-- |
| C1 | `Decode.cpp` 的终局状态选择与参考不等价：会把帧给错音素 | **成立（P0）**。我用 `cl /std:c++20` 直接编真实 `Decode.cpp`（`build/tifa-run/check-decode.cpp`）跑它的反例 `T=1,N=2,groups=[0,1],sim=[[1.0052632,1.2999038]]`：真实输出 `[0,1) [0,0)`（得分 0.50526），参考给第二个音素（0.79990，暴力枚举确认为最优） | **开修**：忠实移植参考 flat 路径（swap 纪律、`next_gap` 更新顺序、被禁 gap 补丁、退出循环不再 swap、终局三路比较、`gap_back` 逐帧后向指针、平局取最小 state 序号）；要求用**真实 C++ vs 真实 Python** 的随机差分自证 |
| C2 | `Scoring.cpp` 建分片列表时多加了 `segment > 0` 过滤，参考只按词号选（`scoring.py:178`） | **结论：真实表上恒等，非缺陷**。参考的 `descriptors` 只对 `mapping > 0` 的行写入（`scoring.py:101-105`），而 `mapping` 只对分歧行非零 → 非哨兵行的 `segment` 恒 ≥ 1，故该过滤在**真实权重产出的表上不可能命中**；它只在**夹具的简化表**上起作用（那是为兼容夹具而写的 shim，已在 `Scoring.cpp` 注释写明依据，并用 `segmentSlots` 判两种布局） | **可选后续**（不是缺陷修复）：把夹具的 `prepare`/`score` 做成参考形状（capacity 带哨兵、padding 片段不带真词号、分歧与第 0 列比），再把 shim 删掉——收益是"DP 在参考形状的表上被验到"，不改也不影响真实路径 |
| C3 | `Decode.cpp` 的平局规则（先 position 后 kind）与参考（最小 state 序号）不同 | 与 C1 同源，未单独剥离 | 随 C1 一起按参考实现 |
| C4 | 零宽归位：`Decode.h` 声明"参考把丢掉的连段移到 groups 允许的 pause 位置、并按左右分段"，原实现恒给 `(0,0)` | **已闭环，无需再改**：`006fd55` 的重写已按该声明实现，代码逐字对应（`Decode.cpp:189-218`：`left = end[lo-1]`、`right = start[hi+1]`、按第一个 `allowed(anchorAt)` 分裂、无处可放则整段压在后边界）；且**可观察行为与参考逐边界相同**（逐 take 口径读数 42/42、逐边界零差异）。注意归位**只移动零宽点、不撑宽度**，上层照旧按 `start >= end` 丢弃——这正是参考 `--skip-handling=omit` 的默认行为。 | **无**（原判"对上层无影响"成立；中途我一度误判为"参考靠归位撑开"，已实测更正——参考自己就丢零宽，`--skip-handling omit`） |
| C5 | 测试覆盖：夹具**没有多候选词**，P3 之前整词 DP 一次都没跑过；`decodeFrames`/`buildGrid` 无直接单测 | 成立（P3 已新增 eng 多候选用例，DP 路径现在会被走到；DP 的**数值正确性**在夹具上仍不可表达——夹具的 cost/tails 恒 0） | 夹具向参考形状靠拢；`decodeFrames` 的差分测试随 C1 补 |

**方法学提示（值得记下来）**：复核者用"C++ 的 Python 转写"当证据，这一步本身不可信；但它的**结构读法** （逐行对比参考的状态机）是对的，我用真实 C++ 一跑就复现了。→ 复核结论必须**用被测语言本身**复验。

### 10.10 后续各期
（待实施；每期一节，含门禁表、缺陷发现与修法、仍未验证项。）

---

## 11. 附录：历史重写与迁移索引（2026-09-30）

> **本节性质**：方案期的迁移过程记录已收束成这一节索引，只留**仍然成立的知识**：那次历史重写做了什么、为什么这么做、旧提交号现在去哪找，以及当时踩过的工具坑。**本节不含可执行的门禁读数**：读数与口径见 §10.8.1，本仓现行状态见 [`docs/otter-design.md`](../otter-design.md)。

**重写做过什么**：2026-09-30 前后，四个仓（otter / ds-editor-lite / synthrt / wolf）的**远端分支被强制重写**（重新撰写的提交序列，不含任何 tifa 工作），本地分支因此都不再位于远端历史内；otter 侧把本地 tifa 增量落到重写后的新结构上（保持单分支），冲突一律按上游的结构与措辞落位。

**为什么这么做**：重写是上游的意图表达——新分支的结构与措辞是权威，本地增量只该**落到新结构上**，不该反向覆盖它；所以那一轮是「纯增量落到新结构」，不是「合并两套同源改动」。

**此后又一次重写（现状）**：本地历史随后**从根重写为 4 个提交**（`88417af` → `bc36a95`），tifa 与包装配的改动都并进这 4 个提交。当前 `analysis-level-1` 与 `origin/analysis-level-1`（`f4820d9`，2026-09-30 的重写头）**没有共同祖先**：`f4820d9` 不再是基线，`git rev-list --count f4820d9..HEAD`、`git diff f4820d9..HEAD` 之类的对账一律作废，不要再照旧引用。

**旧提交号去哪找**：`f4820d9` 之前本地那一串提交号、以及 `safety/*` 安全分支下的号，**都已不在任何 ref 上**（`safety/*` 分支已全部删除）：它们只在 `git reflog` 里，随 reflog 过期或 `git gc` 失效。要留证据就当下摘录进文档，不要指望以后还能 `git show`。

**门禁结论（仍然成立）**：宿主调用 Align 的形态是**一次一个 span**，故等价判据取**逐 take 口径**——当时读数 **42/42 通过、逐边界零差异**；此后**不要再按「零宽音素的去向」或「词典多读的候选选择」去改解码器**，两条路都已实测排除（口径与成因见 §10.8.1）。

**仍在代码里的一条上游修复**：`OnnxSupport.h` 的 `runModel()` 调用自由函数必须写成 `otter::onnx::run(...)`——不限定就会被同名成员函数挡住（name hiding），**所有 Align 变体都编译不过**。该文件属上游，其注释目前引用的是本文删掉的「归档 §11.4」，即本条；接手者可按需把注释改指本节。

**耐久教训（工具与门禁；跟人走不跟版本走）**：

1. **任何门禁读数都必须强制重跑**：门禁脚本原先「输出不存在才跑」，于是把一次中断运行留下的**空文件**读成「该 take 由通过变 0 词」，得出了「解码重写引入回归」的错误结论。
2. **探针的编译期常量指向旧构建树时会静默跑旧产物**：插件目录、驱动目录、ONNX Runtime 都是硬编码常量，换树后不改不重编，读出来的不是当前代码（据此误报过一次「迁移后行为一致」）。
3. **比较器与指标先自证再报数**：判据要能「故意挪两帧必须判红、同输入两次读数逐位一致」；比较器自己启动失败时（f-string 跨行拼接在 Python 3.12+ 是 `SyntaxError`），整批 take 会被记成「失败 + 自检不可用」——整批红时先单独手跑一次比较器。
4. **复核证据要用被测语言本身复验**：把 C++ 逐行转写成 Python 当证据不可信；结构性的读法可以提示方向，结论要用真实构建产物重算。
5. **参照侧的语言码要剥掉批次后缀**（`jpn-syn2` → `jpn`）：否则参考一个文件都不产出，比对读成假失败。
6. **比对素材必须与 oracle 同批**：两侧都吃同一批统一成 48 kHz/16-bit 的副本，拿原始素材去比副本的 oracle 会引入编码差异。
7. **声明上限的实测边界**：`maxSegmentDuration = 60` 接受 60.000 s、超一帧即被拒（当时用的测量脚本在 scratch 目录，不入库）。
8. **清理临时产物前先 grep 文档里的引用**：只有 `build/` 开头的写法会被正则抓到，用反斜杠写、或省略 `build/` 前缀的引用会被漏掉（当时因此删掉了被本文引用的两个工具）。
- **提交卫生**：本地提交的消息正文、作者与提交者身份、新增文件内容三处都不含本机绝对路径或用户名。
