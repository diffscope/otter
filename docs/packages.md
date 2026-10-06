# 包与出厂声明

包是包声明加声明所引用的模型文件，出厂变体有四个：`rmvpe`、`game`、`hfa`、`tifa`。**声明不入库**：本仓不含声明，只有装配与校验它们的脚本与文档，声明本身由打包时用 `--declarations <目录>` 指向打包机上的一份目录（一个变体一个子目录）。发布素材因此与本仓解耦——权重、词表与词典同样不入库：它们都是训练产物，各有自己的来源与再分发条件，与声明一起作为包发布即可，仓库的历史不必跟着模型走。`packages/` 已移出 git 跟踪（`git rm --cached` 加上 `.gitignore` 末尾的 `/packages/`）；本机若仍留着这一份，可以直接把它当作 `--declarations`。

| 声明子目录 | 契约 | 变体 | 模型（打包时复制） |
| :-- | :-- | :-- | :-- |
| `rmvpe/` | `org.openvpi.otter.inference.F0` | `rmvpe` | `rmvpe.onnx`（16 kHz，345 MB） |
| `game/` | `org.openvpi.otter.inference.Note` | `game` | `encoder`、`segmenter`、`estimator`、`bd2dur`、`dur2bd` 五个 ONNX 模型（44.1 kHz） |
| `hfa/` | `org.openvpi.otter.inference.Align` | `hfa` | `model.onnx` 及模型自带的 `config.json`、`vocab.json` 与三部词典（44.1 kHz） |
| `tifa/` | `org.openvpi.otter.inference.Align` | `tifa` | `spectrogram`、`model`、`prepare`、`score`、`select` 五个 ONNX（由上游导出器产出）及导出的 `config.json`、`vocabulary.json` 与四本词典（48 kHz） |

另有两个 ggml 变体 `game-ggml`（`Note`）与 `tifa-ggml`（`Align`），它们**不跑 ONNX 图，而是把引擎自己的 release 当包**：包内必须有引擎的可执行文件、GGUF 权重与引擎自带的 ggml 运行库，声明用 `configuration` 的 `cli`／`model`／`dictionaries` 指到它们。两者**不在 `models-v0.1` 里**，需要单独下载引擎归档（game.cpp 的 `game_ggml-<平台>-<后端>.oudep`、tifa.cpp 的 `tifa-cli-<平台>-<精度>.tar.gz`）；包里没有引擎文件时创建分析器直接失败（`cannot find the game/tifa CLI at <path>`），**不会回退到 ONNX 变体**。安装步骤、平台矩阵、运行代价与示例声明见 [ggml-providers.md](ggml-providers.md)（中文对照 [ggml-providers.zh.md](ggml-providers.zh.md)）。

## 包声明的形状

一个变体一个目录，`desc.json` 位于该目录的根，包的其余内容按声明写的路径摆放：

- `$version`：声明所用的规范版本，当前为 `1.0`。
- `id`：包标识，写作 `otter/<变体>`。
- `version` 与 `compatVersion`：包版本与它可以替换的版本，四个声明取值一致（见下节）。
- `runtimeLevel`：本包依赖的 synthrt Runtime 级别，当前为 `1`。
- `contributions.inference[]`：本包提供的分析模块，每项为 `id`（贡献 id，包内唯一）与 `path`（该模块的清单，相对包根，写作 `./inferences/<id>/inference.json`）。

`inferences/<id>/inference.json` 按 spec 2.4 分成两块，键的含义如下：

- `interface`、`level`、`variant`、`name`：模块实现的契约、契约级别、变体名与显示名。宿主按 `interface` 与 `level` 索取模块，`variant` 说明它由哪个变体实现。
- `exports`：契约面，宿主读取，形状由契约规定并发布为 JSON Schema（`docs/schemas/`）。四个变体都写 `sampleRate`、`channelCount` 与 `maxSegmentDuration`，宿主据此准备音频并按上限切片；F0 另有 `interval`（帧间隔）；Note 与 Align 另有 `languages` 与 `defaultLanguage`（宿主语言的 ISO 639-3 代码，后者为未指定语言时的默认值）；Note 另有 `supportsKnownNotes`；Align 另有 `silenceLabel`，能自行检测非语音的变体（`hfa`）还写 `nonSpeechPhonemes` 与 `defaultNonSpeechPhonemes`。`knobs` 逐旋钮给出取值域与默认值，未列出的旋钮该模块不采用（`tifa` 未声明 `knobs`）。
- `configuration`：变体私有，宿主不读取，解释器据此打开模型：模型文件在包内的相对路径与变体自己的参数（rmvpe 的 `model`；game 的五张图、`timestep` 与语言编号映射；hfa 的 `model`/`config`/`vocab` 与语言编号映射；tifa 的十一处路径与语言编号映射）。

四个声明采用 `inferences/` 布局，`version` 与 `compatVersion` 取值一致，当前均为 `0.1.0.0`。这个值与库版本同源：版本在 `CMakeLists.txt` 的 `project(otter VERSION 0.1.0.0)` 处声明一次，`make-package.py` 从那里读 `manifest.json` 的 `bundleVersion` 并打印本次发布的标签，声明 lint 用同一个值核对四个声明（漂移即报错），所以包声明的版本、批次版本与发布标签是同一个数的三种写法。`--bundle` 可以省略，给了就必须与项目版本相等，写版本 `0.1.0.0` 或写标签 `models-v0.1` 都算相等。发布标签名是该值去掉末尾的零分量得到的形式，因此 0.1.0.0 发布为 `models-v0.1`。

现行发布是标签 `models-v0.1` 下的那一个 release，内含四个包 `otter-game`、`otter-hfa`、`otter-rmvpe`、`otter-tifa`（版本均为 0.1.0.0）与一份列出四者的 `manifest.json`；更早的 `models-v0.1.0.0`、`models-v0.2.0.0`、`models-v0.3.0.0` 均已删除。`game` 包内含 `LICENSE`（1069 B），`hfa` 与 `rmvpe` 不含。

把发布里的包取到本地由 `scripts/fetch-models.cmake` 承担：读 `manifest.json`、按其中记录的 SHA512 校验每个归档、解包到指定目录，**默认只列不拉**（一个 release 有数百 MB）。各 `DOTTER_FETCH_*` 设置与示例见 [README 的 Model packages 一节](../README.md#model-packages)；包内布局、版本与标签规则仍以本文档为准。

`tifa` 的权重取自 TIFA 项目自己的导出：五张 ONNX 图与导出器写出的 `config.json`、`vocabulary.json`、四本词典都不入本仓，装配时用 `--variant tifa --models` 指到那层导出目录。

`tifa` 声明四门语言（`cmn`/`yue`/`jpn`/`eng`），四门都写 `lyrics="scheme"`：歌词由调用方按该语言的 scheme 给出（拼音音节、粤拼音节、罗马字、ARPAbet 词），变体自己不做「文字 → 书写单位」的转换——包里的四本词典只做「书写单位 → 音素」这一步。`lyrics="text"` 是相反的一侧：歌词给普通文字、由变体转换（`hfa` 的英文就是这一侧）。

## 包的装配

包目录即 `desc.json` 所在的目录。`scripts/make-package.py` 按声明把模型复制到位、运行校验、打成 zip 并计算校验和：

```sh
python3 scripts/make-package.py --variant hfa \
    --declarations /path/to/declarations \
    --models /path/to/1218_hfa_model_new_dict \
    --manifest build/packages/manifest.json
```

`--declarations` 指向保存声明的目录（一个变体一个子目录；缺省指仓根下的 `packages/`，而该目录不入库）。`--models` 指向模型文件所在的目录。脚本先按文件在包内的路径查找，未找到时再按**文件名**查找；若包内两个文件同名，脚本拒绝按文件名查找，此时须按包的布局组织 `--models` 目录。同一平台上相同输入的两次打包结果逐字节一致（`scripts/test_make_package.py` 有此项断言）；zip 成员的创建系统字段已固定，压缩仍由 zlib 完成，因此跨平台的字节一致只在 zlib 版本相同时成立。产物写入 `--output`（默认 `build/packages`）：装配好的包目录、`otter-hfa-0.1.0.0.zip`，以及输出到标准输出的 manifest 片段；指定 `--manifest` 时，片段同时并入该文件。`--license` 用来在模型自带的归档里没有许可证时指定要放进包内的 `LICENSE`，`--bundle` 只声明本次发布的版本。

`--models` 是**平铺**的一层：脚本按文件名找，再把文件放到声明写的相对位置上。`tifa` 的十一个文件（五张 ONNX、导出的 `config.json` 与 `vocabulary.json`、四本词典）因此要放在同一层，词典的目录层级由声明决定：

```sh
python3 scripts/make-package.py --variant tifa \
    --declarations /path/to/declarations \
    --models /path/to/tifa-1.0-export-with-dictionaries \
    --manifest build/packages/manifest.json
```

装配的实质操作是复制声明并放入模型，手工装配与之等价：

```sh
cp -r /path/to/declarations/hfa /path/to/otter-hfa
cp /path/to/1218_hfa_model_new_dict/{model.onnx,config.json,vocab.json} /path/to/otter-hfa/
cp /path/to/1218_hfa_model_new_dict/{ds-zh-pinyin-lite,ds_cmudict-07b,japanese_dict_full}.txt /path/to/otter-hfa/
```

## 发布前校验

`make-package.py` 会自动运行以下校验；手工装配时不得省略。声明可以脱离模型单独检查，用 `--declarations-only` 只检查声明本身，不打开声明引用的任何文件：

```sh
python3 scripts/check-declarations.py --declarations-only /path/to/declarations
```

`errors` 必须为 0。参数给出保存声明的目录时，lint 把它当作一组包，逐个子目录（各自有 `desc.json`）检查，上面一条命令即检查四个声明；目录不在任何含 `CMakeLists.txt` 的树内时加 `--no-project`，版本核对随之降为警告。完整模式检查以下各项：声明引用的文件均存在，`configuration` 中的键均为变体读取的键， `exports` 与 `configuration` 相互一致；这些是包声明中最易出错之处。对 `hfa` 另有一组检查，所读取的均为模型自带的文件：`exports` 中的采样率须与模型 `config.json` 的 `mel_spec_config.sample_rate` 一致，声明的每种语言须有词典，非语音标签须在词表中，静音标签须在 `silent_phonemes` 中。对 `tifa` 同样另有一组检查，读的也是模型自带的文件：`exports` 中的采样率须与导出器写的 `config.json` 的 `samplerate` 一致；声明的每种语言须有自己的 `dictionary<Language>` 键且该文件在包里；`phonemes` 须与 `vocabulary.json` 中带该语言前缀的符号逐一相等（无前缀的共享符号如 `AP` 可以出现在任何语言里）；`silenceLabel` 不得与任何声明的音素重名——TIFA 的词表里没有静音符号，这个标签是它给「没有词的片段」的拼写。

## 相对路径的基准

规范 2.4 规定，相对路径以该字段所在声明文件的目录为基准。模型与声明文件不在同一目录，因此 `inferences/*/inference.json` 中的路径写作 `../../<model>.onnx`。若写作 `./<model>.onnx`，路径将指向声明文件自身的目录，校验器报告 `refers to a missing file`。
