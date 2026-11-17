# OTTER：可编排的转写、时值与表情运行时

[English](../README.md)

[synthrt](https://github.com/diffscope/synthrt) 的分析层，为产出音频结构化标注的推理模块定义契约与解释器。

synthrt 定义歌手与推理，[wolf](https://github.com/diffscope/wolf) 加入语言。这两个库都不确定一段录音唱了什么，也就是其中的音符、音高与时值。otter 提供的正是这一层。读入音频、返回其结构化标注的模型，就是接口属于 otter 契约之一的推理模块，包声明它的方式与声明其他模块相同。otter 不增加贡献类别，分析器属于 synthrt 内置的 `inference` 类别，与 wolf 的 G2P 和 S2P 模块一样。

otter 不含可执行程序。它是供编辑器或工具与 synthrt 一起嵌入的库。

## 分析模块

一个在 `desc.json` 中列出分析器的包：

```json
{
  "contributions": {
    "inference": [
      { "id": "f0", "path": "./inferences/f0/inference.json" }
    ]
  }
}
```

每个条目是指向一份分析清单的路径：

```json
{
  "interface": "org.openvpi.otter.inference.F0",
  "level": 1,
  "variant": "rmvpe",
  "name": "RMVPE",
  "exports": {
    "sampleRate": 16000,
    "channelCount": 1,
    "interval": 0.01,
    "maxSegmentDuration": 60,
    "knobs": {
      "voicingThreshold": { "minimum": 0.0, "maximum": 1.0, "default": 0.03 },
      "interpolateUnvoiced": { "default": true }
    }
  },
  "configuration": {
    "model": "../../rmvpe.onnx"
  }
}
```

两个块按 spec 2.4 规定的分工划分。`exports` 属于契约：它声明宿主必须准备的音频格式与模块支持的旋钮，对同一接口的每个变体形式都相同，并以 JSON Schema 的形式发布在 [schemas](schemas) 下。`configuration` 属于变体：它给出模型路径与参数，宿主不读取它。变体在加载时对照检查这两个块，所以声明了模型不支持的采样率的包，或者声明了对齐路径却没有执行它的模型的包，会在任何分析运行之前被拒绝。

Level 1 定义三份契约：

| `interface` | 产出 |
| :-- | :-- |
| `org.openvpi.otter.inference.F0` | 基频曲线，以及每帧的浊音标志 |
| `org.openvpi.otter.inference.Note` | 音符区间，可以以已知的音符为条件 |
| `org.openvpi.otter.inference.Align` | 词与音素的区间，歌词由调用方给出 |

名称 `org.openvpi.otter.inference.Transcribe` 保留给将来的文本转写契约。

## 宿主的职责

otter 不含音频处理代码。它不解码、不重采样、不切片、不读文件，既不依赖 ffmpeg，也不依赖 libsndfile。执行体接受一段连续的 float PCM 以及这段音频的起始时间。

因此宿主负责三件事：

+ **重采样。** 每个模块声明它要求的采样率。各算法的采样率不同（RMVPE 要求 16 kHz，GAME 音符模型与 HFA 对齐器要求 44.1 kHz，TIFA 对齐器要求 48 kHz），所以宿主要读取声明，而不是假定一个采样率。执行体拒绝其他采样率的音频，也从不隐式重采样，因为隐式重采样会在不知不觉中改变结果。声道是例外：执行体会把多个声道平均为一个，因为降混只有一种可能的结果。
+ **切片。** 宿主把音频分成若干段，每段调用一次，每段都不超过模块声明的 `maxSegmentDuration`。
+ **音乐时间。** otter 产出的所有时间值都是绝对秒数。tick、速度与变速曲线属于宿主的时间轴。

## 创建分析器

宿主用 `SynthUnit::setPluginPaths` 把 otter 的插件目录，即安装前缀下的 `lib/synthrt-otter/plugins/inferenceinterpreters`，交给 inference 类别，与 dsinfer 和 wolf 的目录列在一起。CMake 包把每个安装的插件导出为导入目标，例如 `synthrt::otterrmvpe`，需要部署插件或取得插件目录的 CMake 工程用 `$<TARGET_FILE_DIR:synthrt::otterrmvpe>` 读取，vcpkg 安装的 Debug 树也由它解析。分析器不需要发起导入的模块：宿主打开包，取得模块，再经由契约头文件创建分析器。

```cpp
auto spec = package.contribution(srt::InferenceCategory::NAME, "f0");
auto analyzer = otter::Api::F0::L1::createAnalyzer(*spec->as<srt::InferenceSpec>());
```

`createAnalyzer` 用模块的变体构造该契约的 import options 与运行时选项，再调用 `srt::InferenceSpec::createInference`，后者在创建推理之前就拒绝属于其他契约的模块。随后 `createAnalyzer` 检查返回的执行体是否实现了这份契约，不符合时以 `otter::AnalysisError::ContractViolation` 拒绝。其他模块也可以用普通的 `imports` 条目导入分析器，`options` 留空即可。

## 版本与 ABI

**otter 在 1.0 之前不提供任何 ABI 稳定性保证。** 宿主用到的几乎所有类型都是公开头文件中的值类型：`Api/` 下的每个载荷都是，新增虚函数时契约接口的虚表也会变化。混用以两个版本的头文件编译出的对象属于未定义行为，而不是链接错误，因此不会有任何诊断。otter 的版本一变，就要重新构建宿主。

库与插件共用的代码不属于 API。它们是 `src/util` 下的三个私有静态库，按服务的对象命名：`analysisutil` 是库与各插件共用的清单读取函数与检查，`onnxutil` 是 ONNX 提供者共用的会话外壳，`ggmlutil` 是 ggml 引擎用到的文件与管道。头文件不安装，任何二进制都不导出其中的函数。

## 构建要求

CMake 3.20 或更高版本（配置只需要项目声明的 3.19，下文用到的 `ctest --test-dir` 需要 3.20），以及 Git，用于检出 vcpkg 及其 overlay。

包括 synthrt 在内的所有依赖都来自 vcpkg。端口依次从两个 overlay 中查找：先是本仓库的 `scripts/vcpkg-ports`，然后是 `scripts/vcpkg-sources.json` 指定的共享 overlay。

+ [synthrt](https://github.com/diffscope/synthrt)，经由本仓库的 `synthrt-main` 端口，该端口钉在主线的 `onnxruntime-builds-uptake` 分支上
+ [stdcorelib](https://github.com/SineStriker/stdcorelib)
+ [qmsetup](https://github.com/stdware/qmsetup)

只有构建测试时才需要 Boost.Test。

随附的四个 provider 都运行 ONNX 模型，只在有 dsinfer 时构建。`onnx` feature（`--x-feature=onnx`）加入 `synthrt-main[onnx]`，它把 dsinfer 的 CMake 包装在 synthrt 的包旁边，`find_package(synthrt-dsinfer)` 因而能找到它。没有这个 feature 时，库照样构建，包照样加载，只是不能执行分析器。synthrt 在没有自己的驱动时也是这样。`game-ggml` 与 `tifa-ggml` 两个 provider 在两个方向上都不受此限：它们不需要 dsinfer，也不需要 ONNX Runtime，而是**运行**一个来自引擎自己发布版、而非来自本仓库的引擎可执行文件。这个可执行文件从 game.cpp 的 `.oudep` 或 tifa.cpp 的 `tifa-cli-*.tar.gz` 解包得到，由声明指名（见 [ggml-providers.zh.md](ggml-providers.zh.md)）。

## 构建

依赖由 [vcpkg-workspace.cmake](https://github.com/stdware/dev-scripts/blob/main/docs/about-scripts/vcpkg-workspace.md) 安装，该脚本下载到 `scripts/` 中。它把 `scripts/vcpkg-sources.json` 指定的 vcpkg 与 overlay 检出到 `deps/`，并把依赖安装在那里。在仓库根目录执行下列命令。在 Windows 上，把 `curl` 换成 `powershell -Command "Invoke-WebRequest -OutFile scripts/vcpkg-workspace.cmake <url>"` 来下载脚本。

```sh
curl -fsSL -o scripts/vcpkg-workspace.cmake \
    https://raw.githubusercontent.com/stdware/dev-scripts/main/scripts/vcpkg-workspace.cmake
cmake -P scripts/vcpkg-workspace.cmake install -- --x-feature=onnx --x-feature=tests

cmake -B build/cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=deps/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_INSTALLED_DIR=deps/vcpkg/installed \
    -DVCPKG_MANIFEST_MODE=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DOTTER_BUILD_TESTS=ON

cmake --build build/cmake
ctest --test-dir build/cmake
```

模型测试（`test_RmvpeInterpreter`、`test_GameInterpreter`、`test_HfaInterpreter` 与 `test_TifaInterpreter`）需要夹具，夹具是生成的而不是入库的。构建找到带 `onnx` 与 `numpy` 模块的 Python 解释器时，会把夹具生成到构建树中，作为这些测试的依赖；否则这四个测试注册为 disabled，ctest 也如此报告。这个判断在配置阶段作出。已启用的测试在运行时发现数据缺失，会把自己报告为跳过，这是另一种机制（下文连同加载测试的同类区分一起说明）。脚本也可以手动运行：

```sh
python3 scripts/make-model-fixtures.py --output build/cmake/fixtures
```

夹具是真正的 ONNX 图，签名是真实的，只是用简单的运算代替了训练出的权重。它们验证 provider 符合契约，不验证数值精度，后者需要训练好的模型。运行时找不到夹具、驱动插件或 ONNX Runtime 的测试，以 ctest 报告为跳过的状态退出，绝不会报告为通过。

加载测试与运行时测试（`test_AnalysisLoad` 与 `test_AnalysisRuntime`）读取的包目录同样不入库。`scripts/make-test-packages.cmake` 持有它们的内容，一条命令就能准备好：

```sh
cmake -DOTTER_TEST_PACKAGES_OUTPUT=build/test-packages -P scripts/make-test-packages.cmake
```

把这个目录传给构建，测试才能找到它。同一条命令加上 `-DOTTER_TEST_PACKAGES_CHECK=<dir>` 时，会把一个已有目录与脚本中的表对比，报告每个缺失、不同或多余的文件：

```sh
cmake -B build/cmake -G Ninja \
    ... \
    -DOTTER_TEST_PACKAGES_SOURCE="$PWD/build/test-packages"
```

没有这个变量时，这两个测试以 ctest 报告为跳过的状态退出；变量指向一个不存在的目录时，它们失败，所以被告知了包在哪里却找不到包的检出不会悄悄通过。注意，有测试被跳过时 ctest 仍然返回零：它在最后打印有多少测试没有运行，但退出状态保持绿色，所以这个数字是唯一的迹象，CI 不能只看退出状态。像 CI 那样在配置之前生成并检查这个目录，才能保证绿色的结果不会掩盖从未运行的测试。

## 模型包

一个包就是一份声明加上它指名的模型文件，以一个归档的形式随模型一起发布，而不是从本仓库发布：发布版 `models-v0.1` 提供 `otter-game`、`otter-hfa`、`otter-rmvpe` 与 `otter-tifa`，版本均为 0.1.0.0，并附有列出这四个包的 `manifest.json`。声明不在本仓库跟踪，组装时从 `--declarations` 给出的目录读取，一个发布版包含什么以 [packages.md](packages.md) 为准。另有两个变体运行它们自己发布版中的引擎而不是 ONNX 图，打包的是引擎归档而不是权重，[ggml-providers.zh.md](ggml-providers.zh.md) 介绍了它们以及 [examples/packages](examples/packages) 下的声明。

`game-ggml` 与 `tifa-ggml` 是另一种包，这一点值得在这里直接说明：它们驱动的是一个**既不属于本仓库、也不属于模型发布版的外部引擎可执行文件**。它们的包由引擎自己发布版中的引擎归档组装而成，也随包发布这个归档：game 来自 [game.cpp v0.1.3](https://github.com/KakaruHayate/game.cpp/releases/tag/v0.1.3) 的 `game_ggml-<platform>-<backend>[-q8].oudep`，tifa 来自 [tifa.cpp v0.1.6](https://github.com/KakaruHayate/tifa.cpp/releases/tag/v0.1.6) 的 `tifa-cli-<platform>-<precision>.tar.gz`，tifa 的 q4 版是另一个发布版中的 `.oudep`，标签为 [`oudep`](https://github.com/KakaruHayate/tifa.cpp/releases/tag/oudep)。包中放着引擎的可执行文件、GGUF 权重与 ggml 运行库，与经由 `cli`、`model` 和 `dictionaries` 指名它们的声明放在一起。它们不会回退到对应的 ONNX 变体：没有引擎归档的安装在创建分析器时就失败，报 `cannot find the game CLI at <path>` 或 `cannot find the tifa CLI at <path>`，所以宿主只在装有引擎的地方提供这两个变体。安装步骤、平台与后端矩阵以及一次运行的代价见 [ggml-providers.zh.md](ggml-providers.zh.md)。

解包后的引擎放在指名它的声明旁边：示例读取 `../../game_ggml_cli.exe` 与 `../../game_medium.gguf`（game），或 `../../tifa_ggml_cli.exe`、`../../models/tifa.gguf` 与 `../../models`（tifa），所以引擎的可执行文件、权重与 DLL 都与该变体目录中的 `desc.json` 相邻，在本仓库中即 `docs/examples/packages/game-ggml/` 与 `docs/examples/packages/tifa-ggml/`。`scripts/make-package.py` 用它们组装包，`--models` 指向归档解包得到的文件夹，也就是放着引擎可执行文件的那一个：game 的 `.oudep` 就是归档本身解包的位置，tifa 的 `.tar.gz` 则是其中的那个文件夹。

一条命令就能为检出取得真实的包，不需要 Python 解释器：

```sh
DOTTER_FETCH_OUTPUT=build/models DOTTER_FETCH_VARIANTS=hfa \
    cmake -P scripts/fetch-models.cmake
```

脚本读取发布版的 manifest，按 manifest 记录的 SHA512 校验每个归档，再解包校验通过的归档。不指定变体时只列出发布版中有什么，因为一个发布版有几百兆。`DOTTER_FETCH_VARIANTS=all` 取全部包，`DOTTER_FETCH_TAG` 读取另一个发布版，`DOTTER_FETCH_KEEP_ARCHIVES=ON` 把归档保留在包旁边，`DOTTER_FETCH_MANIFEST` 读取已经在磁盘上的 manifest，离线运行就是这样做的。设置先从环境变量读取，再从缓存变量读取。示例使用环境变量，因为实测 cmake 4.3.1 会丢弃这些名字的无类型 `-D` 条目。

组装时复制声明，加入模型文件，写出归档、它的校验和以及一段 manifest：

```sh
python3 scripts/make-package.py --variant hfa \
    --declarations /path/to/declarations \
    --models /path/to/hubertfa-v0.0.7 --manifest build/packages/manifest.json
```

每个包在发布之前都必须通过检查：

```sh
python3 scripts/check-declarations.py <package directory>...
```

`--declarations-only` 只检查声明，不打开它引用的文件，所以打包者可以在模型文件就位之前运行它。

## 使用方法

```cmake
find_package(synthrt-otter CONFIG REQUIRED)
target_link_libraries(example synthrt::otter)
```

分析器是经由 inference 搜索路径找到的插件。所以宿主链接 otter 只是为了契约头文件以及它们调用的库函数，链接本身不做任何注册。

## 文档

+ [otter-design.md](otter-design.md)：设计，包括分层、契约面、宿主职责、决策台账、审计与实施里程碑。
+ [packages.md](packages.md)：工厂声明、包的组装以及包必须通过的检查。
+ [ggml-providers.zh.md](ggml-providers.zh.md)：`game-ggml` 与 `tifa-ggml` 变体，它们以外部进程驱动 game.cpp 与 tifa.cpp 的 ggml 引擎，含引擎下载、平台矩阵与运行代价。英文版为 [ggml-providers.md](ggml-providers.md)。
+ [lite-integration.md](lite-integration.md)：otter 接入 ds-editor-lite 的方式，以及 ds-editor-lite 不再自行实现的功能。
+ [plans/hfa-align.md](plans/hfa-align.md)：Align 契约与 `hfa` 变体的实施计划与记录。
+ [plans/tifa-align.md](plans/tifa-align.md)：`tifa` 变体的实施计划与记录，含真实权重验收的读数。
