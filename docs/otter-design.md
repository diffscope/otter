# OTTER 抽参域方案

**OTTER — Orchestratable Transcription, Timing and Expression Runtime**

本文是 otter 的总方案：钉分层、契约面、宿主责任与实施路径。决策与论证集中在
《[决策台账](#决策台账)》，其余章节只写结论。

## 本文地位与可信源顺序

冲突时按下列顺序裁决，**上位者恒胜**：

1. **[DiffSinger 数据格式与推理接口规范 2.4](../../synthrt/docs/ds-spec-2.4.md)**——上位规范；
2. **synthrt / dsinfer 实际代码**——框架能力的唯一事实来源；
3. **本文**——分层与跨层决策；
4. **[wolf](../../wolf/docs/linguist-architecture.md)**——同构先例，不是规范。wolf 的做法可以参考，
   但不构成 otter 的约束。

## 锚点口径

| 树 | 观察点 | 引用写法 |
| :-- | :-- | :-- |
| spec 2.4 | synthrt 仓 `docs/ds-spec-2.4.md` | `spec 2.4:<行号>` |
| synthrt / dsinfer | `main` `3c7549d` | `synthrt/include/synthrt/Core/ContribSpec.h:120` |
| refactor 抽参子系统 | synthrt `0e3940dc`（lite 当前所钉） | `lib/Extract/...`、`plugins/Extract/...` |
| lite | `origin/main` `a8fac646` | `src/app/Modules/Extractors/...` |

---

## 1. otter 是什么

一个链接进宿主的库，为 synthrt 的 Package 体系新增 **`analysis` 贡献类别**：从音频中得出结构化
标注的模块都归这一类。库本身注册类别、发布契约头、定义提供者抽象；实际跑模型的是它的解释器插件。

Level 1 立两份契约：

| interface | 产出 | 本轮变体 |
| :-- | :-- | :-- |
| `org.openvpi.otter.analysis.F0` | 基频曲线 + 清浊标志 | `rmvpe` |
| `org.openvpi.otter.analysis.Note` | 音符区间（可条件于已知音符） | `game` |

预留两份，本轮不实现、不占用词汇：

| interface | 产出 |
| :-- | :-- |
| `org.openvpi.otter.analysis.Align` | 音素 / 词的时间区间（强制对齐） |
| `org.openvpi.otter.analysis.Transcribe` | 文本（ASR） |

## 2. otter 不是什么

- **不是音频库。** 不解码、不重采样、不切片、不读写文件，不依赖 ffmpeg / libsndfile。输入是一段
  已经就绪的 `float` PCM。
- **不是程序。** 与 wolf 一样，是给编辑器或工具链接的库。
- **不负责时间轴。** 一律输出**绝对秒**，不知道 tick、不知道 BPM、不知道变速曲。

## 3. 与 refactor 抽参子系统的差异

lite 当前经 `srt-audio` + `srt-extract` + `plugins/Extract/{rmvpe,game}` 抽参。otter 相对它的
处置：

| refactor | otter | 说明 |
| :-- | :-- | :-- |
| `lib/Audio`（~1700 行，ffmpeg/swresample） | **丢弃** | 解码与重采样归宿主（lite 的 talcs） |
| `lib/Audio/Slicer`（~230 行 RMS 切片） | **交给宿主** | 见 A7 |
| `lib/Extract/AudioPreprocessor` | **丢弃** | 只做重采样 + 切片，两件事都归宿主了 |
| `lib/Extract/{Pitch,Midi}Extractor` 接口 | **重写为契约** | 见第 5 章 |
| `lib/Extract/ExtractorDriver` | **丢弃** | `InferenceDriver` 是 `RuntimeService`，直接取（A3） |
| `plugins/Extract/rmvpe`（~350 行） | **移植** | 推理与 `interpF0` 逻辑保留，外壳重写 |
| `plugins/Extract/game`（~1030 行） | **移植 + 补回** | 四阶段推理保留；补回 refactor 丢掉的 `dur2bd` session（A11） |
| 模型 = 磁盘上的裸文件 | **spec 2.4 Package** | 见第 6 章 |
| 输出 tick（480 PPQ，单一 tempo） | **绝对秒** | 见 A5；这同时修掉一个真实缺陷 |

已知缺陷，移植时一并修正：

- **`GameExtractor` 丢了 `dur2bd` session。** 旧 `GameModel` 有它，把已知音符时长转成边界喂给
  segmenter；refactor 迁移时未保留，`known_boundaries` / `prev_boundaries` 从此恒为全零。align
  能力在模型里，在代码里不见了。
- **`language` 读了等于没读。** `GameExtractor::open()` 在 `config.contains("languages")` 分支里
  直接 `m_language = 0`。
- **tick 用单一 tempo 换算。** lite 传 `timeline.tempoAt(0)`，变速曲的音符位置系统性偏移。
- **`uv` 名实不符。** refactor 注释写「true=浊音 voiced」，字段却叫 `uv`（unvoiced）。otter 改名
  `voiced`，语义为 1=浊音（A6）。

## 4. 分层与框架落点

### 4.1 为什么必须自注册类别

`srt::ContribSpec` 不提供公开的执行体入口；`ContribCategory::createExecutiveFactory()` 只接受
`ContribImportBinding`，即**一份贡献只有被别的模块 `imports` 才能变成执行体**
（`synthrt/include/synthrt/Core/ContribCategory.h:131`、`ContribImportBinding.h:15`）。框架里唯一
的顶层入口是 `SingerPipelineExtension` 那种 `ContribSpecExtension`
（`synthrt/include/synthrt/SVS/SingerPipelineExecutive.h:21`）。

抽参器没有任何模块 import 它——宿主自己就是调用方。因此 otter 必须：

1. 注册自己的类别 `analysis`，
2. 定义自己的 `ContribSpecExtension` 作为顶层入口。

这与 wolf 的 A1 结论相反，原因也正在此：wolf 的 G2P 被 linguist 模块 import，otter 的抽参器不被
任何东西 import。

### 4.2 不绕 `inference` 中转

`ds::InferenceDriver` 派生 `srt::RuntimeService`（`dsinfer/include/dsinfer/Inference/InferenceDriver.h:65`），
任何类别的解释器都能经 `spec.package().synthUnit().runtimeService(ds::InferenceDriverPlugin::IID, "onnx")`
取到——wolf 的 `multig2p-onnx` 就是这么干的（`src/plugins/inferenceinterpreters/multig2p/main.cpp:473`）。
所以 `analysis` 模块直接跑 ONNX，不需要再 import 一层 `inference` 模块。

### 4.3 类型分层

```
otter (库，链接进宿主)
├── Analysis/AnalysisContrib.h      类别与声明：AnalysisCategory / AnalysisSpec
├── Analysis/AnalysisExecutive.h    顶层入口：AnalysisExtension / AnalysisExecutive
├── Analysis/AnalysisProvider.h     提供者抽象（自带契约守卫，见 A14）
├── Analysis/AnalysisProviderPlugin.h  插件 IID
├── Analysis/AnalysisTask.h         执行生命周期：建于 srt::ITask 之上的任务面（A15 及补记）
├── Analysis/AnalysisError.h        取消与工作线程失败的错误类别（A18）
├── Support/ManifestValues.h        声明读取器，两个提供者共用
└── Api/
    ├── Common/1/CommonApiL1.h      AudioSegment、Knob、ProgressCallback
    ├── F0/1/F0ApiL1.h              org.openvpi.otter.analysis.F0 契约面
    └── Note/1/NoteApiL1.h          org.openvpi.otter.analysis.Note 契约面

otter 插件（独立动态库，嵌 AnalysisProviderPlugin::IID）
├── analysisproviders/rmvpe        (F0, 1, "rmvpe")
└── analysisproviders/game         (Note, 1, "game")
```

`analysis` 类别**不追加任何字段**。宿主在设置界面列出已装抽参器时需要的信息——`name`、
`interface`、`variant`——全在 spec 2.4 的公共字段里，`DataOnly` 模式即可读到。类别不加字段就不必
为将来的 Align / Transcribe 再改类别（A2）。

### 4.4 生命周期

```
SynthUnit::openPackage(...)                       // 装模型包
  └─ AnalysisCategory 解析声明 → AnalysisSpec
       └─ 解释器（插件）挂上 AnalysisExtension

宿主:
  spec->exports()->as<F0Schema>()                 // 读能力：采样率、帧间隔、旋钮域
  ContribSpecExtension::findFromSpec<F0Executive>(*spec)
      ->as<AnalysisExtension>()
      ->createAnalyzer(runtimeOptions)            // → unique_ptr<AnalysisExecutive>
      ->as<F0Executive>()
  executive->start(input)    /  startAsync(input, cb)
  executive->stop()          /  waitForFinished()
```

执行体必须在其 Package 释放前销毁——与 `ContribExecutive` 的通用规则一致。

## 5. 契约面

### 5.1 公共（`Api/Common/1`）

```cpp
namespace otter::Api::Common::L1 {

    /// 一段连续 PCM，由宿主准备好。
    struct AudioSegment {
        /// 采样率（Hz）。必须等于模块 exports 报出的 sampleRate，否则执行失败。
        int sampleRate = 0;
        /// 声道数。多于模块 exports 报出的 channelCount 时降混，少于则拒绝。
        int channelCount = 0;
        /// 交错排列的样本。宿主移入，执行期间由 StartInput 持有。
        std::vector<float> samples;
        /// 这段音频在宿主时间轴上的起点（秒）。结果按它锚定。
        double startTime = 0;
    };

    /// 执行进度，取值 0 到 1。可以不给。
    using ProgressCallback = std::function<void(double)>;

    /// 一个连续旋钮的声明：本模块认不认、域是多少、默认多少。
    struct Knob {
        bool honored = false;
        double minimum = 0;
        double maximum = 0;
        double defaultValue = 0;
    };

    struct IntKnob {
        bool honored = false;
        int minimum = 0;
        int maximum = 0;
        int defaultValue = 0;
    };

    struct FlagKnob {
        bool honored = false;
        bool defaultValue = false;
    };

}
```

### 5.2 `org.openvpi.otter.analysis.F0` Level 1

```cpp
namespace otter::Api::F0::L1 {

    inline constexpr char API_INTERFACE[] = "org.openvpi.otter.analysis.F0";
    inline constexpr int API_LEVEL = 1;

    /// 本模块公开的能力。宿主据此准备音频、生成设置界面。
    class F0Schema : public srt::ContribExports {
    public:
        /// 模型要求的输入采样率（Hz）。宿主负责重采样到这个值。
        int sampleRate = 0;
        /// 模型要求的声道数。
        int channelCount = 1;
        /// 输出帧之间的时间间隔（秒）。RMVPE 为 0.01。
        double interval = 0;
        /// 单次执行能接受的最长音频（秒）。0 表示无上限。
        double maxSegmentDuration = 0;

        /// 清浊判定阈值。
        Common::L1::Knob voicingThreshold;
        /// 是否把清音段的 f0 插值填上。
        Common::L1::FlagKnob interpolateUnvoiced;
    };

    /// 读取声明的 exports 块。契约语法归接口所有，每个 variant 都经此一处读取，再各自核对
    /// 自己的模型能否兑现。configuration 块归 variant 所有，其类型定义在各自的 provider 里。
    OTTER_EXPORT srt::Expected<std::unique_ptr<F0Schema>>
        readF0Schema(const srt::ContribSpec &spec, std::string variant);

    class F0RuntimeOptions : public otter::AnalysisRuntimeOptions { /* 空 */ };

    /// 一次执行的输入。旋钮不填即用模块默认值。
    class F0StartInput : public srt::TaskStartInput {
    public:
        Common::L1::AudioSegment audio;
        Common::L1::ProgressCallback progress;

        std::optional<double> voicingThreshold;
        std::optional<bool> interpolateUnvoiced;
    };

    /// 一次执行的结果。覆盖整段输入，按绝对时间锚定。
    class F0Result : public srt::TaskResult {
    public:
        /// 第一帧对应的绝对时间（秒），等于输入的 startTime。
        double startTime = 0;
        /// 相邻帧的时间间隔（秒）。
        double interval = 0;
        /// 基频（Hz）。清音帧的取值由 interpolateUnvoiced 决定。
        std::vector<float> f0;
        /// 与 f0 等长；1 表示浊音。
        std::vector<uint8_t> voiced;
    };

    class F0Executive : public otter::AnalysisExecutive {
    public:
        using AsyncCallback =
            std::function<void(srt::Expected<std::unique_ptr<F0Result>>)>;

        virtual srt::Expected<std::unique_ptr<F0Result>> start(const F0StartInput &input) = 0;
        virtual srt::Expected<void> startAsync(std::shared_ptr<const F0StartInput> input,
                                               AsyncCallback callback) = 0;
    };

}
```

### 5.3 `org.openvpi.otter.analysis.Note` Level 1

```cpp
namespace otter::Api::Note::L1 {

    inline constexpr char API_INTERFACE[] = "org.openvpi.otter.analysis.Note";
    inline constexpr int API_LEVEL = 1;

    /// 转写出的一个音符。
    struct NoteInfo {
        /// MIDI 音高编号。
        int key = 0;
        /// 绝对起点（秒）。
        double start = 0;
        /// 时长（秒）。
        double duration = 0;
        /// 置信度，0 到 1。
        double confidence = 0;
    };

    /// 已知音符，用于把转写条件在既有结果上（对齐模式）。
    struct KnownNote {
        double start = 0;
        double duration = 0;
    };

    class NoteSchema : public srt::ContribExports {
    public:
        int sampleRate = 0;
        int channelCount = 1;
        /// GAME 为 60。宿主切片时不得超过它。
        double maxSegmentDuration = 0;

        /// 本模块认得的语言标识。空表示不区分语言。
        std::vector<std::string> languages;
        /// 是否接受 knownNotes。
        bool supportsKnownNotes = false;

        /// 音符边界的判定阈值。
        Common::L1::Knob boundaryThreshold;
        /// 相邻边界的最小间隔（秒）。
        Common::L1::Knob boundaryRadius;
        /// 音符置信阈，送入模型。
        Common::L1::Knob noteThreshold;
        /// 输出筛选阈：置信度低于它的音符不出现在结果里。
        Common::L1::Knob notePresenceCutoff;
        /// 迭代步数。
        Common::L1::IntKnob steps;
    };

    /// 读取声明的 exports 块，同 F0。game variant 的 configuration（五个模型路径、timestep、
    /// 语言编号映射、扩散起点）是它自己的类型，宿主不读。
    OTTER_EXPORT srt::Expected<std::unique_ptr<NoteSchema>>
        readNoteSchema(const srt::ContribSpec &spec, std::string variant);

    class NoteRuntimeOptions : public otter::AnalysisRuntimeOptions { /* 空 */ };

    class NoteStartInput : public srt::TaskStartInput {
    public:
        Common::L1::AudioSegment audio;
        Common::L1::ProgressCallback progress;

        /// 语言标识。不填即用模块默认。
        std::optional<std::string> language;
        std::optional<double> boundaryThreshold;
        std::optional<double> boundaryRadius;
        std::optional<double> noteThreshold;
        std::optional<double> notePresenceCutoff;
        std::optional<int> steps;

        /// 已知音符，时间为宿主时间轴上的绝对秒（与 audio.startTime 同一基准，X3）。空表示自由转写。
        std::vector<KnownNote> knownNotes;
    };

    class NoteResult : public srt::TaskResult {
    public:
        /// 按起点升序，时间为绝对秒。
        std::vector<NoteInfo> notes;
    };

    class NoteExecutive : public otter::AnalysisExecutive {
    public:
        using AsyncCallback =
            std::function<void(srt::Expected<std::unique_ptr<NoteResult>>)>;

        virtual srt::Expected<std::unique_ptr<NoteResult>> start(const NoteStartInput &input) = 0;
        virtual srt::Expected<void> startAsync(std::shared_ptr<const NoteStartInput> input,
                                               AsyncCallback callback) = 0;
    };

}
```

### 5.4 旋钮与三层的对应

spec 2.4 把声明文件分三层，三个字段各就各位：

| 内容 | 落点 | 谁读 | 例 |
| :-- | :-- | :-- | :-- |
| 宿主要看懂的能力与旋钮域 | `exports` → `Schema` | 宿主 | `sampleRate`、`maxSegmentDuration`、`boundaryThreshold` 的域与默认值 |
| 只有解释器关心的实现参数 | `configuration` → `Configuration` | 解释器 | 模型路径、`timestep`、语言编号映射、扩散起点 |
| 每次执行的可调值 | `StartInput` | 调用方填 | `boundaryThreshold = 0.3` |

判据仍是 spec 2.4 §《何时递增 Level》那条：**导入方需要看懂它吗**。`timestep` 改了模型就跑不对，
它是「模型是什么」不是「参数」，进 `configuration`；`boundaryThreshold` 用户直接感知松紧，进契约。

旋钮一律 `std::optional`：不填即用模块默认值。这样将来加旋钮时，不填新旋钮的旧宿主行为不变
（A9）。

## 6. 模型包形状

抽参模型按 spec 2.4 打成 Package，与声库包共用同一套 `SynthUnit` 与搜索路径（A12）。

```
+ rmvpe-1.0.0
  + analyzers
    + f0
      - analysis.json
      - rmvpe.onnx
  - desc.json
```

`desc.json`：

```json
{
  "$version": "1.0",
  "id": "openvpi/rmvpe",
  "version": "1.0.0.0",
  "compatVersion": "1.0.0.0",
  "contributions": {
    "analysis": [
      { "id": "f0", "path": "./analyzers/f0/analysis.json" }
    ]
  }
}
```

`analysis.json`：

```json
{
  "interface": "org.openvpi.otter.analysis.F0",
  "level": 1,
  "variant": "rmvpe",
  "name": { "_": "RMVPE", "zh-CN": "RMVPE 音高提取" },
  "exports": {
    "sampleRate": 16000,
    "channelCount": 1,
    "interval": 0.01,
    "knobs": {
      "voicingThreshold": { "minimum": 0.0, "maximum": 1.0, "default": 0.03 },
      "interpolateUnvoiced": { "default": true }
    }
  },
  "configuration": {
    "model": "./rmvpe.onnx"
  }
}
```

GAME 同理，`contributions.analysis` 一项，`interface` 为 `org.openvpi.otter.analysis.Note`，
`exports` 多出 `languages`（标识列表）、`supportsKnownNotes` 与五个旋钮，`configuration` 列四到
五个 session 的路径、`timestep` 与语言标识到模型编号的映射。两个契约的 `exports` 与
`imports[].options` 各有一份 JSON Schema，在 `docs/schemas/`。

## 7. 插件形状

按 spec 2.4 §3，插件嵌 IID `org.openvpi.otter.plugin.AnalysisProvider`，同目录 `plugin.json`：

```json
{
  "name": "otterrmvpe",
  "interpreters": [
    { "interface": "org.openvpi.otter.analysis.F0", "level": 1, "variant": "rmvpe" }
  ]
}
```

宿主为 `analysis` 类别配一条插件搜索路径：`SynthUnit::setPluginPaths("analysis", {...})`。

## 8. 宿主责任

otter 把三件事明确留给宿主，缺一不可：

1. **重采样。** 宿主读 `Schema::sampleRate` / `channelCount`，把音频转到那个格式。RMVPE 要
   16000/1，GAME 要 44100/1——两个算法不同，宿主必须按模块报的值来，不能写死。执行体校验输入，
   不匹配直接失败，不会私自重采样。
2. **切片。** 宿主用 RMS 静音切片把音频分段，每段不超过 `Schema::maxSegmentDuration`，逐段调用，
   每段填对 `audio.startTime`。otter 不切片（A7）。
3. **时间轴换算。** otter 输出绝对秒，宿主按自己的 timeline 转成 tick，变速曲由宿主负责。

对 lite 而言：重采样交给 talcs；切片器需要从 `f807eaea^` 的 `src/libs/audio-util` 捞回来（那次
提交把它和 `game-infer`/`rmvpe-infer` 一起删了，lite 现在没有 RMS 切片器）。两个算法共用一套切片
参数（A8）。

## 9. 决策台账

### A1 — otter 自注册 `analysis` 类别，不复用 `inference`

**决策**：otter 注册新贡献类别 `analysis`，并定义自己的 `ContribSpecExtension` 作为顶层执行体入口。

**依据**：`ContribSpec` 无公开执行体入口；`ContribCategory::createExecutiveFactory()` 只接受
`ContribImportBinding`（`ContribCategory.h:131`）；框架唯一的顶层入口是
`SingerPipelineExtension`（`SingerPipelineExecutive.h:21`）。抽参器不被任何模块 import，只能自建
入口，而扩展点的 traits 以 spec 类型为键，故必须有自己的 spec 类型，也就必须有自己的类别。

**备选与否决理由**：
- *把抽参器做成 `inference` 贡献*——`InferenceSpec` 同样只能经 import 建执行体，宿主拿不到顶层
  入口。否决。
- *让抽参模块 import 一个 `inference` 模块（wolf 的 linguist 做法）*——wolf 需要它是因为 linguist
  是 G2P/S2P/Onset 的**组合**；抽参器就是单个模型，多一层间接层只增加包结构复杂度。且 A3 表明
  取驱动不需要这一层。否决。

**代价（已接受）**：宿主要多配一条 `analysis` 的插件搜索路径。

### A2 — `analysis` 类别不追加字段

**决策**：类别层不加任何字段，只用 spec 2.4 的公共字段。

**依据**：类别追加字段的意义是「解释器选出来之前就要读到」（spec 2.4《模块》三层表）。宿主在设置
界面列已装抽参器时要的是 `name` / `interface` / `variant`，全是公共字段。

**代价（已接受）**：宿主无法在 `DataOnly` 模式下读到采样率等能力——那些在 `exports` 里，要 `Load`
之后才有。可接受：抽参器数量少，宿主本来就全装全载。

### A3 — 直接取 `InferenceDriver`，不经 `inference` 中转

**决策**：解释器经 `spec.package().synthUnit().runtimeService(ds::InferenceDriverPlugin::IID, "onnx")`
拿 ONNX 驱动。

**依据**：`ds::InferenceDriver` 派生 `srt::RuntimeService`（`InferenceDriver.h:65`），与类别无关；
wolf 的 `multig2p-onnx` 已是此做法（`multig2p/main.cpp:473`）。refactor 的
`srt::extract::getInferenceDriver(runtime)` 是那条线上的产物，随 `srt-core` 一起丢弃。

### A4 — 一个类别装下 F0 / Note / Align / Transcribe

**决策**：四份契约同属 `analysis` 类别，靠 `interface` 区分。

**依据**：spec 2.4 说「输入输出已根本不同就另起一份 `interface`」，而不是另起一个类别；类别的扩展
点是「需要一种全新的模块」。四者都是「模块声明 + 解释器 + 顶层执行体」，形状相同。

**代价（已接受）**：宿主枚举 `analysis` 贡献时要按 `interface` 筛。

### A5 — 时间一律用绝对秒（`double`）

**决策**：所有对外时间量为 `double` 秒——`Note::start` / `duration`、`F0Result::interval` /
`startTime`、`AudioSegment::startTime`、`boundaryRadius`。不出现 tick，不出现毫秒。

**依据**：dsinfer 的 L1 契约全线如此（`CommonApiL1.h` 的 `InputNoteInfo::duration`、
`InputParameterInfo::interval`、`InputPhonemeInfo::start`）。同一宿主里两套时间单位是长期负担。
且模型本来就产出秒（GAME 的 `durations` 是秒），整数化会引入累加漂移——今天 tick 那条路径正是
这么偏的。

**备选与否决理由**：
- *int 毫秒*——跨语言绑定更好传，但逐音符累加时 1ms 量化在长句上会漂，且与 dsinfer 契约不同口径。
  否决。
- *音符用毫秒、f0 用秒*——同一套契约两种单位，读的人要多记一件事。否决。

### A6 — 清浊标志叫 `voiced`，1 表示浊音

**决策**：字段名 `voiced`，`uint8_t`，1 = 浊音。

**依据**：refactor 的字段叫 `uv`（unvoiced）而注释写「true=浊音 voiced」，名实相反，是明确的缺陷
诱因。`std::vector<bool>` 同时换成 `std::vector<uint8_t>`——前者的位压缩特化不能取地址、不能安全
地跨线程按元素写。

### A7 — 切片归宿主，otter 不切

**决策**：执行体只接受一段连续音频，不做静音切片。宿主负责分段并逐段调用。`Schema` 报出
`maxSegmentDuration` 作为硬约束。

**依据**：用户口径。切片需要的信息（编辑区域、可见范围、用户选区）在宿主手里，宿主切得更准；且
otter 因此完全不需要音频侧代码。

**代价（已接受）**：`Schema` 不报切片参数，将来若出现某个算法确实需要不同切片参数的情形，补进
`exports` 词汇表要递增 Level。接受这个代价，而不是现在为一个被明确告知不要读的消费方预留词汇。

### A8 — 两个算法共用一套切片参数

**决策**：宿主用一套切片配置，不按算法分。

**依据**：换算成时间后两套参数本来就几乎相同——`hopSize` 都是 10ms（160@16k / 441@44.1k），
`winSize` 都是 40ms，`minInterval` 都是 300ms，`maxSilKept` 都是 500ms，阈值都是 0.02；只有
`minLength` 不同（RMVPE 5s / GAME 2s）。取一套即可，GAME 的 60s 上限由宿主保证不超。

### A9 — 旋钮一律 `std::optional`，缺省即模块默认

**决策**：`StartInput` 里每个旋钮是 `std::optional<T>`；不填时解释器用声明 `exports.knobs` 里的默认
值（A17 之后默认值随旋钮域一起归契约）。`Schema` 同时报出该旋钮本模块认不认、域是多少、默认多少；
未声明的旋钮忽略调用方的取值。

**依据**：与 dsinfer 的 `AcousticStartInput::depth` + `AcousticConfiguration::useVariableDepth`
同一做法。宿主可据 `Schema` 自动生成设置界面；新算法多一个旋钮，不填它的宿主行为不变。

**备选与否决理由**：
- *开放 JSON 字典*——新算法加参数宿主一行不改，但编译期类型安全尽失，且各算法同一件事可能用不同
  键名，契约等于没说。否决。
- *变体私有参数一律固化在 `configuration`，用户不可调*——契约最小最稳，但用户想调阈值就得重发模型
  包。与「尽量都可调」的要求冲突。否决。

### A10 — 进入契约的旋钮清单

**决策**：

| 契约 | 旋钮 | 对应的现有硬编码 |
| :-- | :-- | :-- |
| F0 | `voicingThreshold` | RMVPE `RmvpeExtractor.cpp:103` 的 `0.03f` |
| F0 | `interpolateUnvoiced` | RMVPE 无条件调用的 `interpF0()` |
| Note | `boundaryThreshold` | `seg_threshold`，送 segmenter 的 `threshold` |
| Note | `boundaryRadius` | `seg_radius_seconds`，除 `timestep` 得 `radius` 帧 |
| Note | `noteThreshold` | `est_threshold`，送 estimator 的 `threshold` |
| Note | `notePresenceCutoff` | `buildMidiNotes` 里的 `presence > 0.5f` |
| Note | `steps` | `generateD3pmTs()` 写死的 `n_steps = 8` |
| Note | `language` | `config.json` 的 `languages`（改字符串标识） |
| Note | `knownNotes` | segmenter 的 `known_boundaries` / `prev_boundaries` |

**依据**：逐个核对了两个插件送进 ONNX session 的全部输入（segmenter：`x_seg`、`maskT`、
`known_boundaries`、`prev_boundaries`、`language`、`threshold`、`radius`、`t`；estimator：
`x_est`、`boundaries`、`maskT`、`maskN`、`threshold`；RMVPE：`waveform`、`threshold`）。`t`（扩散
步长）由 `steps` 与 `configuration` 的 `scheduleStart` 生成，不整表暴露。

**落地时改的**：`t` 是**批次维**而不是步数维——真实导出的每个 segmenter 输入共享同一个批次维，`t` 也
在内，所以一次调用只带一个时间步，**采样循环在宿主这边**，逐步把上一步的输出当 `prev_boundaries` 喂
回去。三个旋钮（segmenter 的 `threshold` / `radius`、estimator 的 `threshold`）是**标量**，秩不符会被
拒绝而不是广播。estimator 的 `presence` 输出是**布尔**，按张量自带的类型读成 1 / 0 的置信度，所以
`notePresenceCutoff` 的 0.5 仍然分得开。

**留在 `configuration` 的**：`timestep`、`scheduleStart`、语言编号映射、各 session 路径——改了模型
就跑不对，属「模型是什么」。`sampleRate` 与旋钮默认值按 A17 归 `exports`。

### A11 — `language` 用字符串标识，映射写在 `configuration`

**决策**：契约里 `language` 是字符串（如 `"zh"`、`"ja"`）；模型内部的 int 编号由
`NoteConfiguration::languages` 映射。

**依据**：int 编号是某一个模型的内部约定，两个模型的 `1` 未必是同一种语言。契约词汇必须跨变体稳定。
dsinfer 的 `AcousticConfiguration::languages`（`std::map<std::string, int>`）是同一做法。

### A12 — 对齐模式经 `knownNotes` 表达，`dur2bd` session 补回

**决策**：`NoteStartInput::knownNotes` 非空即对齐模式；解释器经 `dur2bd` 把已知时长转成边界，喂
segmenter 的 `known_boundaries`。`NoteSchema::supportsKnownNotes` 声明本模块认不认。

**依据**：能力在模型里——旧 `GameModel` 有 `runDur2bd` 与 `InferenceInput::known_durations`；
refactor 的 `GameExtractor` 迁移时丢了这个 session，`inferSlice` 里 `knownBoundaries` 恒为全零。
移植时按旧实现补回。

**代价（已接受）**：ASR / 强制对齐（音素级）不走这条路，它们是另外两份 `interface`，本轮只留位置。

### A13 — 抽参模型包与声库包共用同一 `SynthUnit`

**决策**：宿主用同一个 `SynthUnit`、同一套 Package 搜索路径装载两者，按类别区分。

**依据**：spec 2.4 的 Package 体系本就统一；两个 unit 要维护两套生命周期，且抽参模型将来可能被
声库包 `dependencies` 引用（例如某声库自带专用的音高模型）。

**代价（已接受）**：`SynthUnit::loadedPackages()` 会同时包含两类包，宿主枚举时要按 contributions
的类别筛。

### A14 — 提供者不得把扩展挂到自己不服务的声明上

**决策**：`AnalysisProvider` 的构造函数接收它服务的 (interface, level, variant)，
`createExtensions` 声明为 `final`，先用 `serves()` 守卫再委托给 `createAnalysisExtension()`。

**依据**：`PackageLoader::attachSpecExtensions` 把**每一个已注册解释器**的 `createExtensions`
作用到**每一份正在加载的声明**上（`synthrt/lib/Core/PackageLoader.cpp:955`）。这是 spec 2.4 有意
为之——别的类别可以为新 role 供给执行体。无条件接受的提供者会把扩展挂满整个包，宿主向一份声明索取
它没有的契约时会拿到一个不属于它的执行体，而不是 nullptr。

**代价（已接受）**：提供者不能自己决定扩展的挂载条件。真需要时可以改成受保护的钩子，但那要等到
真的有这种需求。

**发现方式**：`test_AnalysisLoad` 断言 f0 声明上找不到 Note 扩展。两个提供者当时都挂满了。

### A15 — 取消的归属在调用方线程上确定

**决策**：`AnalysisRunner::begin()` 在调用方线程上认领执行并清掉上一次的取消标志；工作线程只跑
函数体，不再清标志。同步与异步两条入口都先 `begin()`。

**依据**：把清标志放在「工作开始处」对异步入口是错的——`stop()` 完全可能在 `startAsync()` 返回之后、
工作线程进入函数体之前到达，那一次取消会被清掉。测试里这条必现。

**代价（已接受）**：多一个类。但三个执行体否则要各写一遍同一段并发代码，而这段代码的错法是静默的。

**补记（2026-09-13）**：`AnalysisRunner` 与 `srt::ITask` 重复了状态、取消标志、工作线程与等待四样东西，
第四轮审计记为冗余。现以 `otter::AnalysisTask<Input, Result>` 取代：它派生自 `srt::ITask`，状态与等待
直接用框架的 `m_state` / `m_asyncState`，只覆写 `startAsync()` 以保住本条决策的两个规则——认领与清
标志在调用方线程上进行；回调结束后才释放执行，且回调内启动的下一次执行按代数接管 running 状态而不是
被当成并发拒绝。同步入口 `start()` 用同一份 running 状态认领，`waitForFinished()` 因此对同步与异步一并
等待。任务由契约类（`F0Executive` / `NoteExecutive`）持有并实现 `start` / `startAsync` / `state` /
`stop` / `waitForFinished`；提供者只写受保护的 `run()`，需要连带停止模型会话的提供者覆写 `stop()` /
`waitForFinished()` 并先调基类。X5 所述「建线程失败后永久拒绝」在新实现里同样由 `startAsync()` 自行释放
认领。

### A16 — 进度回调放在 `StartInput`

**决策**：`ProgressCallback` 是 `StartInput` 的字段，不是 `RuntimeOptions` 的。

**依据**：进度是一次执行的属性，不是执行体的属性——同一个执行体被复用于多段音频时，每段的进度
接收方可能不同。main 线没有进度回调先例，此处属新增；放在 per-call 的位置代价最小。

### A17 — `exports` 归契约，`configuration` 归变体，接口名带项目段

**决策**：采样率、声道数、帧间隔、段上限、语言标识、`supportsKnownNotes` 与全部旋钮域写在声明的
`exports` 里，由库中每契约一个的读取函数（`readF0Schema` / `readNoteSchema`）解析，并发布
JSON Schema（`docs/schemas/`）。`configuration` 只剩变体私有内容：rmvpe 的模型路径；game 的五个模型
路径、`timestep`、语言编号映射、`scheduleStart`。变体在加载期把两块互相核对：模型跑不了的采样率、
声明了却没有模型的对齐路径，都在加载期拒绝。接口名改为 `org.openvpi.otter.analysis.F0` /
`.Note`，扩展 ID 改为 `org.openvpi.otter.extension.*`。

**依据**：spec 2.4 §「三个语法块的归属」——`exports` 语法归 `interface + level`、由导入方读，
`configuration` 全部归 `variant`；「需要参与跨模块契约的内容应由 `exports` 公开，而不是作为
`configuration` 中的契约字段」。此前两个变体把契约事实放进各自的 `configuration` 再派生 `exports`，
第三个变体就得为同一批事实另起一套键，DataOnly 读不到能力，README 与本文的示例又与 lint 相反。
接口名对齐 `org.openvpi.dsinfer.inference.*` 与 `org.openvpi.wolf.inference.*` 的项目段写法，
趁尚无已发布包时改掉。

**取代**：`F0Configuration` / `NoteConfiguration` 两个接口头里的配置类型（变体各自定义），
以及「声明不得写 exports」的 lint 规则。

### A18 — 取消与工作线程失败走 otter 自己的错误类别

**决策**：新增 `otter::AnalysisError`（`Cancelled` / `NoWorker`）与其 `std::error_category`
（`otter/Analysis/AnalysisError.h`）。被 `stop()` 中止的执行以 `AnalysisError::Cancelled` 返回错误、
`state()` 报 `Canceled`；`startAsync()` 建不出工作线程以 `NoWorker` 返回。

**依据**：synthrt 的 `Error.h` 明言框架的错误码枚举不由上层库扩展，上层库注册自己的类别；此前三个
提供者用 `InvalidArgument` 表示取消，宿主只能靠 `state()` 区分取消与失败，且 spec 2.4:602 要求契约
公布其错误条件。现在契约的错误条件即本类别的两个值加框架的 `InvalidFormat` / `FeatureNotSupported`。

**代价（已接受）**：多一个头与一个 `.cpp`；宿主比较 `code()` 时多认一个类别。

## 9.5 联合审计（实施后）

实施完成后对 otter / synthrt / wolf 三层做了一轮联合审计，维度为稳定性、向后兼容、规范符合度、
并行安全。synthrt 与 wolf 无新发现（各自测试 16/16、17/17）。otter 发现七项并全部修复，每项都补了
能复现原缺陷的用例。

| 编号 | 类别 | 问题 | 处置 |
| :-- | :-- | :-- | :-- |
| **X1** | 正确性 | `channelCount` 读进成员后从不使用，两个提供者无条件降混。声明 2 声道的模型会被静默喂进单声道 | 校验下沉到 `otter::prepareSamples()`，模型声明的声道数本构建无法满足时直接拒绝 |
| **X2** | 正确性 | `samples.size()` 未校验整帧数，尾部半帧被整除丢弃 —— 表现为很久以后的漂移而不是错误 | 同上，拒绝而非丢弃 |
| **X3** | 契约 | `knownNotes` 的时间基：头文件说「与音频同一时间轴」（绝对），实现按相对整段起点处理，而输出是绝对的。宿主分析第一段看不出差别，之后每一段都错且无提示 | 统一为绝对；提供者减去 `startTime` 并校验落在段内。用例改为 `startTime = 30` |
| **X4** | 规范符合度 | `createImportOptions` 无条件报错，把 spec 2.4 允许的情形变成加载失败 —— 规范明确允许 import 一个不提供 Factory 的类别的贡献，加载器也接受空工厂 | 返回空 options + 惰性 binding；只有真写了 options 才拒绝（没人会读它） |
| **X5** | 健壮性 | `AnalysisRunner::spawn` 建线程抛异常时 `running` 永久为真，分析器此后拒绝一切执行 | 捕获并自行释放认领 |
| **X6** | 契约 | Schema 声明了旋钮取值域但无人校验，`steps = -1` 会生成空张量喂给模型 | `otter::chooseKnob()` 按声明的域校验，越界拒绝而不是钳位 |
| **X7** | 测试有效性 | 桩提供者不做任何校验，与真实提供者行为不同 —— 针对桩写的契约用例因此说明不了契约 | 桩改为调用同一套库函数；X1/X2/X6 的用例正是先在桩上红掉才暴露的 |

X1、X2、X6 的共性是同一件事：两个提供者各写一遍校验，写法必然分叉。和 `AnalysisTask` 同理，
校验也下沉进库（`otter/Analysis/AnalysisInput.h`），三个实现从此不可能不一致。

## 10. 实施里程碑

| 里程碑 | 内容 | 状态 |
| :-- | :-- | :-- |
| **M1** | 仓库骨架：CMake、vcpkg 清单与 port | **完成**。`find_package(otter)` 从安装树可用，消费者不引用任何符号仍拿到类别 |
| **M2** | 类别与契约头 | **完成**。`test_AnalysisContrib` / `test_AnalysisLoad` / `test_AnalysisRuntime` |
| **M3** | `rmvpe` 提供者 | **完成**。`test_Rmvpe`，跑在真实 ONNX 图上 |
| **M4** | `game` 提供者，补回 `dur2bd` | **完成**。`test_Game`，含对齐路径 |
| **M5** | 打包 lint 与模型 fixture | **完成**。`scripts/check-declarations.py`、`scripts/make-model-fixtures.py` |
| **M6** | lite 接入 | **完成**。L1–L6 六步全部落地，见 [lite-integration.md](lite-integration.md) |

### 已验证与未验证

**已验证**：类别注册、包加载（含 `DataOnly`）、解释器发现、扩展点挂载、执行体创建、同步与异步执行、
取消、输入校验、旋钮透传、两个提供者在真实 ONNX 图上的完整链路、对齐路径确实跑了 `dur2bd`、
`find_package(otter)` 从安装树消费、无 dsinfer 的最小构建优雅降级。

**已用真实权重验证的**：GAME。三个合成音 A3 / C4 / E4 转录回 MIDI 57 / 60 / 64，边界落在秒上，时长
与占空比吻合。这一次也正是它暴露出上面那几条形状假设是错的——**fixture 原先照着提供者的假设声明形状，
所以测试永远绿而任何真实导出都跑不了**。fixture 现在按真实导出写。

**未验证**：RMVPE 的数值。权重尚未到齐；相关用例在缺 fixture 时跳过而不是假装通过。

## 11. 未决

- **`Align` / `Transcribe` 的契约面。** 本轮只登记 `interface` 名与所属类别，不定义类型。
- **是否需要 `AnalysisSession`。** wolf 有 `LinguistSession` 承载目录、就绪度与执行体池。otter 的
  抽参器数量少、一次只跑一个，暂不做。lite 接入后样板确实重复了（取分析器、读 schema、备音频、切片、
  逐片跑），但两个 Task 的后半段——结果形状与时间轴换算——并不共用，下沉之前要先想清楚下沉的是什么。
- **跨段边界连续性。** 早先这里写着「segmenter 的 `prev_boundaries` 本可由上一段的结果填充」。
  **这是误读，已由真实模型证伪**：`prev_boundaries` 是**采样循环自己的状态**，每一步把上一步的输出喂
  回去，与上一段音频无关。跨片连续性在这个模型里没有现成的入口。
