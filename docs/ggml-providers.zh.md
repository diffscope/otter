# ggml 提供者（中文对照）

> 本文是 [ggml-providers.md](ggml-providers.md) 的中文对照，两份文件内容对应同一件事；若两者不一致，以英文版为准（英文版是本仓的正式文档）。

出厂变体把 ONNX 模型跑在 dsinfer 里。另有两个变体：`game-ggml` 与 `tifa-ggml`，它们**把 [game.cpp](https://github.com/KakaruHayate/game.cpp) 与 [tifa.cpp](https://github.com/KakaruHayate/tifa.cpp) 的 ggml 引擎当作外部进程驱动**。它们提供与 ONNX 变体相同的 Level 1 契约——`org.openvpi.otter.inference.Note` 与 `org.openvpi.otter.inference.Align`——因此宿主可以把两者放在同一个设置页里，包则通过在清单里点其中一个变体名来选择后端。

提供者从不接触引擎的权重与 ggml 后端：包指名引擎的命令行工具，提供者启动它、交给它一个片段，再把答案读回来。game 引擎按分析器常驻一个 `serve` 进程（标准输入上一个二进制请求帧，标准输出上一行 JSON 音符）；tifa 引擎每次执行跑一条 `align` 命令，用 TextGrid 回答。引擎需要的一切——可执行文件、它那份发布里的 ggml DLL、权重、词典——都留在宿主进程之外，位置就是引擎自己那份发布的布局。

## 先看清"外挂"这件事

+ **引擎不是本仓的一部分，也不在模型发布里**：`models-v0.1` 只有四个 ONNX 包（`otter-game`、`otter-hfa`、`otter-rmvpe`、`otter-tifa`）。要用 ggml 变体，必须**单独去引擎自己的 release 下载引擎归档**。
+ **包 = 引擎归档 + 声明**：包里必须有引擎的可执行文件、GGUF 权重、引擎自带的 ggml 运行库（tifa 还要词典树）。声明用 `configuration` 的 `cli`、`model`、`dictionaries` 指到它们。
+ **没有回退**：缺少引擎可执行文件时，创建分析器直接失败——`cannot find the game CLI at <path>` 或 `cannot find the tifa CLI at <path>`——**不会悄悄改用 ONNX 变体**。所以宿主只应在已经装好引擎的环境里提供这两个变体。
+ **引擎归档本身就是一个 OpenUtau 依赖包**：里面有 `oudep.yaml`，其 `entrypoints` 是 `loader: Executable`，`path` 指引擎可执行文件。

## 安装引擎

引擎是引擎自己发布的归档，安装方式与 [docs/packages.md](packages.md) 里的模型包相同：把归档解包到任何位置，再让声明指名解包出来的文件。

**解包到哪**：两份示例都从声明向上两级读它们指名的文件（`../../game_ggml_cli.exe`、`../../models/tifa.gguf`），所以解包出来的内容要落在 `desc.json` 旁边：

```text
docs/examples/packages/tifa-ggml/            docs/examples/packages/game-ggml/
  desc.json                                    desc.json
  inferences/align/inference.json              inferences/note/inference.json
  tifa_ggml_cli.exe                            game_ggml_cli.exe
  models/tifa.gguf                             game_medium.gguf
  models/dictionaries/, models/cpp_pinyin/     config.json, ggml*.dll
  ggml*.dll, msvcp140*.dll, vcruntime140*.dll
```

### 对照版本

本文与 `docs/examples/packages` 下的示例声明对照过下面这两份发布；换版本时请同时核对引擎的 `config.json` 与本文以下各节。

| 引擎 | 发布 | 本文对照的归档 |
| :-- | :-- | :-- |
| game.cpp | **[v0.1.3](https://github.com/KakaruHayate/game.cpp/releases/tag/v0.1.3)** | [`game_ggml-windows-x64-vulkan.oudep`](https://github.com/KakaruHayate/game.cpp/releases/download/v0.1.3/game_ggml-windows-x64-vulkan.oudep)（Windows x64 / Vulkan / 非量化） |
| tifa.cpp | **[v0.1.6](https://github.com/KakaruHayate/tifa.cpp/releases/tag/v0.1.6)** | [`tifa-cli-windows-x64-full.tar.gz`](https://github.com/KakaruHayate/tifa.cpp/releases/download/v0.1.6/tifa-cli-windows-x64-full.tar.gz)；q4 是另一条发布标签 [`oudep`](https://github.com/KakaruHayate/tifa.cpp/releases/tag/oudep) 下的 `tifa-ggml-<平台>-q4.oudep` |

更新的引擎发布很可能可用，但以上各键仍以那份发布自己的 `config.json` 与 `--help` 为准。示例与本文的文件清单按 Windows 命名（实测就在 Windows 上做的）；其它平台的归档带自己的可执行文件与运行库名，`cli` 要指到那份归档里的可执行文件。

### 平台与后端

| 引擎 | 归档名 | 可选的平台 / 后端 |
| :-- | :-- | :-- |
| game.cpp | `game_ggml-<平台>-<后端>[-q8].oudep` | 平台 `windows-x64`、`linux-x64`、`macos-x64`、`macos-arm64`；后端 `vulkan`、`cuda`、`metal`、`cpu`（Windows 只发布 `vulkan` 与 `cuda`）；`-q8` 是量化版，体积更小 |
| tifa.cpp | `tifa-cli-<平台>-<精度>.tar.gz`（CUDA 版写作 `tifa-cuda-<平台>-<精度>.tar.gz`） | 平台 `windows-x64`、`linux-x64`、`macos-arm64`；精度 `full`、`q4` |

### 最小可运行文件清单

| 变体 | 同一目录下必须有 |
| :-- | :-- |
| `game-ggml` | `game_ggml_cli.exe`、`game_medium.gguf`（约 190 MB）、`ggml*.dll`（含后端 DLL，如 `ggml-vulkan.dll`）、`config.json` |
| `tifa-ggml` | `tifa_ggml_cli.exe`、`models/`（`tifa.gguf` 约 80 MB、`dictionaries/`、`cpp_pinyin/`、按需的 `breath-*.gguf`）、`ggml*.dll`，Windows 上还有引擎自带的 MSVC 运行时 DLL（`msvcp140*.dll`、`vcruntime140*.dll`） |

日文汉字歌词需要额外的词典资产：引擎自带 README 说明要把 v0.1.6 发布里的 [`unidic-lite-dicdir.zip`](https://github.com/KakaruHayate/tifa.cpp/releases/download/v0.1.6/unidic-lite-dicdir.zip) 解压为 `models/unidic/`（假名歌词开箱即用）。

### 声明的 `configuration` 键

声明的配置块指名这些文件，路径相对于声明自身：

| 变体 | 契约 | `configuration` |
| :-- | :-- | :-- |
| `game-ggml` | `Note` | `cli`（引擎可执行文件）、`model`（GGUF）、`languages`（语言标识 → 模型里的语言编号）、`timestep` |
| `tifa-ggml` | `Align` | `cli`、`model`、`dictionaries`（G2P 词典树）、`languages`（语言标识 → 引擎语言码） |

`docs/examples/packages` 下有两份按真实发布写的示例声明：`game-ggml`（GAME-1.0-medium）与 `tifa-ggml`（TIFA-1.0-ST）。它们原样通过的是 `--declarations-only` 声明级 lint（只查声明、不打开声明指名的文件）：引擎文件不在本仓，完整模式必然把它们报成缺失。`make-package.py` 按装配出厂包的方式装配它们——把 `--models` 指向**归档解包后含引擎可执行文件的那层目录**（game 的 `.oudep` 解包是平的，tifa 的 `.tar.gz` 解包后里面还有一层），脚本会把引擎、模型与词典复制进包：

```sh
python3 scripts/make-package.py --variant game-ggml \
    --declarations docs/examples/packages \
    --models /path/to/unpacked/game_ggml-windows-x64-vulkan-q8 \
    --manifest build/packages/manifest.json
```

**声明不指名引擎的运行库**（Windows 上工具运行时加载的那些 `ggml*.dll`），所以只按声明装配出来的包缺这些 DLL；归档里的 `config.json` 与 `oudep.yaml` 同理——声明从不指名它们，只有整份解包目录才带着。最省事的装法是先解包引擎归档，再把装配好的 `desc.json` 与 `inferences/` 拷进解包目录——整个目录既是引擎安装，也是 otter 包。

**引擎的 `config.json` 是权威**：解包后的 game 归档里写着 `samplerate 44100`、`timestep 0.01`、`languages {en:1, ja:2, yue:3, zh:4}`，声明里的 `sampleRate`/`timestep`/`languages` 必须与之一致，否则会得到"看着合理但速度或语言错"的结果。

tifa 的词典以命令行的 `--dict-dir` 传给引擎，对应声明的 `dictionaries`，而它指的是**引擎的模型目录** `models/`（`dictionaries/`、`cpp_pinyin/` 与那几个松散的 G2P `.txt` 都在这一层），因为引擎会在给定目录**下面**再找 `dictionaries/…`。要避免的错误是写成 `models/dictionaries`：引擎会去找 `models/dictionaries/dictionaries/…` 并报词典缺失。不写这个键就不传参数，引擎按自己的默认位置找。

导出与模型不一致的声明仍会通过 lint，因为 lint 读不了 GGUF。lint 查不了的部分由宿主负责：导出项的 `sampleRate` 必须是模型训练时的采样率（GAME 44100，TIFA 48000），否则引擎会以错误的速度给出看似合理的结果。`scripts/read-ggml-metadata.py` 会打印声明需要从 GGUF 里读到的信息——音频格式、game 模型的语言编号、tifa 词表覆盖的每种语言的音素：

```sh
python3 scripts/read-ggml-metadata.py --variant game <model.gguf>
python3 scripts/read-ggml-metadata.py --variant tifa <model.gguf>
```

## 构建

这两个提供者不需要 dsinfer、不需要 ONNX Runtime、也不需要引擎的源码，所以默认与其余插件一起构建，不必额外配置：

```sh
cmake -B build/cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
    ... \
    -DOTTER_BUILD_GAME_GGML=ON -DOTTER_BUILD_TIFA_GGML=ON
```

两个选项默认都是开；关掉一个就把那个提供者排除在部署之外。两者之间唯一共用的代码是 `otter_cli_support`——那个启动子进程、把片段写成 float32 WAV、并管理一次执行的 scratch 目录的私有静态库。

## 宿主可观察到的行为

ggml 变体与它们的 ONNX 同胞共享契约面，只在宿主能看见的几点上不同：

+ 引擎调用是整体的：game 变体先报 0 再报 1；tifa 变体报 0、0.7、1。停止会杀掉引擎进程——game 变体下一次执行要重新启动它并再付一次模型加载，tifa 执行则在它的工具消失时立刻报取消。
+ `game-ggml` 没有对齐路径，因为 serve 协议不为"把已知时长换算成边界"的模型开会话，所以它的导出不能声明 `supportsKnownNotes`，给了已知音符的执行会被拒绝。
+ `tifa-ggml` 对多音字取 G2P 给出的第一条读音，ONNX 变体会拿读数与音频打分再选一条。需要别的读音时，调用方按该词典条目携带的 scheme 拼写写歌词。
+ `game-ggml` 用固定值给引擎的采样随机性播种，因此在**每次新建分析器**的前提下，同一片段的重复转写逐字节一致。但在**同一个常驻分析器**内，实测同一片段的第 1 次执行与后续几次不同（某素材上 23 个音符对 22 个）——要每次拿到同一答案的宿主请每次新建分析器，或不要假设首次等于后续。引擎文档称它把种子 0 当作请求随机，这一点在 v0.1.3 的 Vulkan 构建上没有复现：种子 0 与种子 1 各自都完全可复现，两者的差别只在音高和的末几位。
+ 引擎自己的进度信息写到宿主的标准错误，GUI 宿主可能想要捕获它。
+ 它的 `maxSegmentDuration` 与 ONNX 同胞不同：`game-ggml` 声明 30 秒而 `game` 声明 60 秒，超过的跨度会被引擎直接拒绝（`this model accepts at most 30.000000 seconds in one execution`）；按 60 秒切片的宿主在这个变体上必须按 30 秒切。

## 运行代价

+ tifa 变体**每次执行都新起一个进程**并重新加载模型：两点法实测（Windows x64、v0.1.6 构建），固定开销约 **0.55 秒**（进程启动 + 模型加载），外加每 1 秒音频约 **0.11 秒**——7.4 秒的片段 1.4 秒、39 秒的片段 5.0 秒（含引擎推理本身）。按小节频繁调用 align 时会反复付这笔固定开销；未来若做常驻 `serve` 模式可省掉它。
+ 引擎 I/O **没有超时**：引擎卡住时宿主会一直等下去，取消只能靠杀进程。
+ tifa 的每次执行都会写一份临时 WAV 与一个歌词文本到 scratch 目录，执行结束时删除。

## 实测记录

2026-10-06 的跨提供者实测（同一素材、两侧同跑，ONNX 侧 CPU EP、ggml 侧 Vulkan）显示：这两个变体与 ONNX 同胞在合约的时间分辨率（10 ms 帧）上高度一致。

+ Align：合成素材 6 例中 4 例两侧结果逐字节相同，1 例仅 1 处边界差 1 帧，1 例（英文）只有 4 个多音字的读音不同；真实演唱素材 11 例中 7 例逐字节相同，3 例各有 2–4 处音素边界差 1 帧，1 例仅末段时长差 1 µs。
+ Note：音符起点差 ≤2 帧（20 ms）、音高多为一致或差 1 个半音；game 变体因固定种子在**每次新建分析器**时完全可复现，而 ONNX 变体（`segmenter.onnx` 的 `/RandomUniformLike` 无种子）在真实演唱素材上 11 例中有 5 例两次运行连音符数都不同。
+ 上限差异：note 契约的 `maxSegmentDuration` 与 ONNX 变体不同（30 秒 vs 60 秒），超过 30 秒的跨度会被引擎直接拒绝（`this model accepts at most 30.000000 seconds in one execution`）。
