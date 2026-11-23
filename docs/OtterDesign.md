# OTTER 抽参域方案

**OTTER — Orchestratable Transcription, Timing and Expression Runtime**

本文是 otter 的总体方案，规定分层、契约面、宿主责任与实施路径。决策及其依据集中在《[决策台账](#9-决策台账)》，其余章节只陈述结论。

## 本文地位与可信源顺序

各来源发生冲突时按下列顺序裁决，**上位来源优先**：

1. **[DiffSinger 数据格式与推理接口规范 2.4](https://github.com/diffscope/synthrt/blob/main/docs/ds-spec-2.4.md)**：上位规范；
2. **synthrt / dsinfer 实际代码**：框架能力的唯一事实来源；
3. **本文**：分层与跨层决策；
4. **[wolf](../../wolf/docs/LinguistArchitecture.md)**：同构先例，不属于规范。wolf 的做法可供参考，但不构成 otter 的约束。

## 锚点口径

| 代码树 | 观察点 | 引用写法 |
| :-- | :-- | :-- |
| spec 2.4 | synthrt 仓 `docs/ds-spec-2.4.md` 的 main 分支（[在线](https://github.com/diffscope/synthrt/blob/main/docs/ds-spec-2.4.md)） | `spec 2.4 §<章节名>` |
| synthrt / dsinfer | 分支 `main`（由 `scripts/vcpkg-ports/synthrt-main` 固定提交） | 文件路径与符号，如 `synthrt/include/synthrt/Core/ContribSpec.h` |
| refactor 抽参子系统 | lite `main` 分支所引用的 synthrt 版本 | `lib/Extract/...`、`plugins/Extract/...` |
| lite | 分支 `main` | `src/app/Modules/Extractors/...` |

---

## 1. 定位

otter 是链接进宿主的库，为 synthrt 的 inference 模块定义一组**分析契约**。从音频中得出结构化标注的模块，均为接口属于这些契约之一的 inference 模块。库本身发布契约头文件并定义解释器抽象；模型由解释器插件执行。otter 不注册自有的贡献类别（A26）。

Level 1 定义三份契约：

| interface | 产出 | 变体 |
| :-- | :-- | :-- |
| `org.openvpi.otter.inference.F0` | 基频曲线与清浊标志 | `rmvpe` |
| `org.openvpi.otter.inference.Note` | 音符区间（可以已知音符为条件） | `game` |
| `org.openvpi.otter.inference.Align` | 词与音素的时间区间（强制对齐，歌词由调用方给出） | `hfa`、`tifa` |

另预留一份契约，本轮不实现，也不定义其词汇：

| interface | 产出 |
| :-- | :-- |
| `org.openvpi.otter.inference.Transcribe` | 文本（ASR） |

## 2. 范围之外

- **音频处理。** otter 不解码、不重采样、不切片、不读写文件，不依赖 ffmpeg 或 libsndfile。输入是宿主已准备好的 `float` PCM。
- **独立程序。** 与 wolf 相同，otter 是供编辑器或工具链接的库。
- **时间轴。** otter 一律输出**绝对秒**，不涉及 tick、BPM 与变速曲。

## 3. 与 refactor 抽参子系统的差异

lite 此前经 `srt-audio`、`srt-extract` 与 `plugins/Extract/{rmvpe,game}` 抽参。otter 对其各部分的处置如下：

| refactor | otter | 说明 |
| :-- | :-- | :-- |
| `lib/Audio`（约 1700 行，ffmpeg/swresample） | **丢弃** | 解码与重采样归宿主（lite 中为 talcs） |
| `lib/Audio/Slicer`（约 230 行，RMS 切片） | **交由宿主** | 见 A7 |
| `lib/Extract/AudioPreprocessor` | **丢弃** | 其职能仅为重采样与切片，二者均归宿主 |
| `lib/Extract/{Pitch,Midi}Extractor` 接口 | **重写为契约** | 见第 5 章 |
| `lib/Extract/ExtractorDriver` | **丢弃** | `InferenceDriver` 是 `RuntimeService`，可直接获取（A3） |
| `plugins/Extract/rmvpe`（约 350 行） | **移植** | 保留推理与 `interpF0` 逻辑，重写外壳 |
| `plugins/Extract/game`（约 1030 行） | **移植并补回** | 保留四阶段推理；补回 refactor 遗漏的 `dur2bd` session（A12） |
| 模型为磁盘上的裸文件 | **spec 2.4 Package** | 见第 6 章 |
| 输出 tick（480 PPQ，单一 tempo） | **绝对秒** | 见 A5；同时修正下述 tick 换算缺陷 |

refactor 中的已知缺陷，移植时一并修正：

- **`GameExtractor` 缺少 `dur2bd` session。** 旧 `GameModel` 包含该 session，用于将已知音符时长转换为边界并送入 segmenter；refactor 迁移时未保留该 session，`known_boundaries` / `prev_boundaries` 因此恒为全零。模型具备对齐能力，而代码未使用该能力。
- **`language` 读取后未生效。** `GameExtractor::open()` 在 `config.contains("languages")` 分支中直接赋值 `m_language = 0`。
- **tick 按单一 tempo 换算。** lite 传入 `timeline.tempoAt(0)`，变速曲中的音符位置因此系统性偏移。
- **`uv` 名实不符。** refactor 的注释为「true=浊音 voiced」，字段名却为 `uv`（unvoiced）。otter 将其改名为 `voiced`，语义为 1 表示浊音（A6）。

## 4. 分层与框架落点

### 4.1 分析器作为 inference 模块

`srt::InferenceSpec::createInference(importOptions, runtimeOptions)` 是公开方法，只要求模块已载入、两份选项与模块的 (interface, level, variant) 一致，不要求该模块被其他模块 import （`synthrt/include/synthrt/SVS/InferenceContrib.h`、`lib/SVS/InferenceContrib.cpp`）。因此宿主可以直接在顶层创建分析器，方式与 wolf 的测试直接创建 G2P/S2P 执行体相同。每份契约的 `Executive` 类提供 `create(spec)`，该函数用模块自身的 variant 构造该契约的两份选项，调用 `createInference`，再以 `dynamic_cast` 检查并转换为该契约的执行体；解释器返回其他契约的执行体时报 `AnalysisError::ContractViolation`（A30 修订：原报 `Internal`）。其他模块仍可以用普通的 `imports` 条目（`options` 为空）引用分析器，此时框架以相同方式代为创建分析器。

分析器与 wolf 的 G2P/S2P 共用 inference 类别，插件安装在 `lib/synthrt-otter/plugins/inferenceinterpreters`，嵌入 synthrt 的 `InferenceInterpreterPlugin::IID`。宿主经 `SynthUnit::setPluginPaths(srt::InferenceCategory::NAME, ...)` 交给该类别的序列中包含 `prefix / "lib/synthrt-otter/plugins/inferenceinterpreters"`，与 dsinfer 和 wolf 的同名目录并列。CMake 包把每个安装的插件导出为导入目标 `synthrt::<目标名>`，CMake 工程用 `$<TARGET_FILE_DIR:...>` 取得插件所在目录。

### 4.2 ONNX 驱动的直接获取

`ds::InferenceDriver` 派生自 `srt::RuntimeService`（`dsinfer/include/dsinfer/Inference/InferenceDriver.h`），任何解释器都能经 `spec.package().synthUnit().runtimeService(ds::InferenceDriverPlugin::IID, "onnx")` 取得该驱动，wolf 的 `multig2p-onnx` 即采用此方式。因此分析模块直接运行 ONNX 模型，无须再 import 另一个 inference 模块。

### 4.3 类型分层

```
otter (库，链接进宿主)
├── Analysis/AnalysisExecutive.h    AnalysisExecutive（派生 srt::InferenceExecutive）与两类选项的基类
├── Analysis/AnalysisError.h        取消、工作线程失败、内部错误、契约违规、模型失败与模型不匹配的错误类别（A18、A28、A30）
├── Analysis/Provider/              提供者面：解释器实现者在树内或作为第三方变体读取的头，契约头也按值包含其中的 AnalysisTask.h（A31）
│   ├── AnalysisInterpreter.h       解释器抽象（派生 srt::InferenceInterpreter，拒绝非空 import options）
│   ├── AnalysisTask.h              执行生命周期：建于 srt::ITask 之上的任务面（A15 及补记），
│   │                               非模板基类 AnalysisTaskBase 在库内实现（A28）
│   └── AnalysisInput.h             输入校验：PreparedSamples::prepare()、Knob::choose()（第 9.5 节）
└── Api/
    ├── Common/1/CommonApiL1.h      AudioSegment、Knob / IntKnob / FlagKnob、ProgressCallback
    ├── F0/1/F0ApiL1.h              org.openvpi.otter.inference.F0 契约面
    ├── Note/1/NoteApiL1.h          org.openvpi.otter.inference.Note 契约面
    └── Align/1/AlignApiL1.h        org.openvpi.otter.inference.Align 契约面

src/util 下的私有静态库（不安装，链接它们的库与插件各自静态链接，A28、A32、A33）
├── analysisutil                    库与各插件共用
│   ├── ManifestValues.h            声明读取器
│   ├── F0Curve.h                   清音插值 interpolateUnvoiced()
│   └── KnownNotes.h                knownNotes 检查 checkKnownNotes()
├── onnxutil/OnnxSupport.h          四个 ONNX 提供者共用的会话外壳，仅在含 dsinfer 的构建中存在
└── ggmlutil                        两个 ggml 提供者共用
    ├── EngineFiles.h               交给引擎的 float32 WAV 与一次执行的 scratch 目录
    └── EnginePipe.h                引擎进程（stdc::Popen）的启动设置与管道读写

otter 插件（独立动态库，嵌入 srt::InferenceInterpreterPlugin::IID）
├── inferenceinterpreters/rmvpe    (F0, 1, "rmvpe")
├── inferenceinterpreters/game     (Note, 1, "game")
├── inferenceinterpreters/hfa      (Align, 1, "hfa")
└── inferenceinterpreters/tifa     (Align, 1, "tifa")
```

测试桩不在插件树里。`src/tests/auto/plugins/stub` 把 F0 / Note / Align 三契约的桩编成 OBJECT 库 `otterstub`，以静态插件注册，不依赖 dsinfer，由 `test_AnalysisLoad` 与 `test_AnalysisRuntime` 链接（A33）。

inference 类别**不追加任何字段**。宿主在设置界面列出已安装抽参器所需的信息（`name`、 `interface`、`variant`）均属于 spec 2.4 的公共字段，`DataOnly` 模式即可读取。宿主按 `interface` 从 inference 模块中筛选出 otter 的契约；新增契约只需增加头文件与解释器，不涉及任何类别。

### 4.4 生命周期

```
SynthUnit::openPackage(...)                       // 装载模型包
  └─ InferenceCategory 解析声明 → InferenceSpec
       └─ 解释器（插件）读取 exports 与 configuration

宿主:
  F0Api::Exports::read(spec)                       // 读取能力：采样率、帧间隔、旋钮域
  F0Api::Executive::create(*spec->as<InferenceSpec>())
      // = spec.createInference(ImportOptions(), RuntimeOptions())
      // → unique_ptr<Executive>
  executive->start(input)    /  startAsync(input, cb)
  executive->stop()          /  waitForFinished()
```

执行体必须在其 Package 释放前销毁，这与 `ContribExecutive` 的通用规则一致。

## 5. 契约面

以下代码为契约的摘要形式，完整定义与注释以 `include/otter/Api/` 下的头文件为准。

### 5.1 公共部分（`Api/Common/1`）

```cpp
namespace otter::Api::Common::L1 {

    /// 一段连续 PCM，由宿主准备。
    struct AudioSegment {
        /// 采样率（Hz）。必须等于模块 exports 声明的 sampleRate，否则执行失败。
        int sampleRate = 0;
        /// 声道数。多于模块 exports 声明的 channelCount 时降混，少于时拒绝。
        int channelCount = 0;
        /// 交错排列的样本，样本数必须为整帧。宿主移入，执行期间由 StartInput 持有。
        std::vector<float> samples;
        /// 该段音频在宿主时间轴上的起点（秒）。结果以此为锚点。
        double startTime = 0;

        /// 该段的时长（秒）；无样本时为 0。
        double duration() const noexcept;
    };

    /// 执行进度，取值 0 到 1。可以为空。返回 false 表示请求停止，执行体按其他停止请求的方式处理：以 AnalysisError::Cancelled 返回错误；请求到达前已产出结果的执行保留该结果。
    using ProgressCallback = std::function<bool(double)>;

    /// 一个连续旋钮的声明：本模块是否采用、取值域与默认值。
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

### 5.2 `org.openvpi.otter.inference.F0` Level 1

```cpp
namespace otter::Api::F0::L1 {

    inline const srt::ContribContract Contract{"org.openvpi.otter.inference.F0", 1};

    /// 本模块公开的能力。宿主据此准备音频、生成设置界面。
    class Exports : public srt::ContribExports {
    public:
        /// 模型要求的输入采样率（Hz）。宿主负责重采样到该值。
        int sampleRate = 0;
        /// 模型要求的声道数。
        int channelCount = 1;
        /// 输出帧之间的时间间隔（秒）。RMVPE 为 0.01。
        double interval = 0;
        /// 单次执行接受的最长音频（秒）。0 表示无上限。
        double maxSegmentDuration = 0;

        /// 清浊判定阈值。
        Common::L1::Knob voicingThreshold;
        /// 是否对清音段的 f0 进行插值填充。
        Common::L1::FlagKnob interpolateUnvoiced;

        /// 读取声明的 exports 块。契约语法归接口所有，每个 variant 均经此函数读取，再各自核对
        /// 自身模型能否满足。
        static srt::Expected<std::unique_ptr<Exports>> read(const srt::ContribSpec &spec);
    };

    class ImportOptions : public otter::AnalysisImportOptions { /* 只标识契约 */ };
    class RuntimeOptions : public otter::AnalysisRuntimeOptions { /* 只标识契约 */ };

    /// 一次执行的输入。未填的旋钮取模块默认值。
    class StartInput : public srt::TaskStartInput {
    public:
        inline static const auto &Contract = L1::Contract;

        Common::L1::AudioSegment audio;
        Common::L1::ProgressCallback progress;

        std::optional<double> voicingThreshold;
        std::optional<bool> interpolateUnvoiced;
    };

    /// 一次执行的结果。覆盖整段输入，以绝对时间为锚点。
    class Result : public srt::TaskResult {
    public:
        inline static const auto &Contract = L1::Contract;

        /// 第一帧对应的绝对时间（秒），等于输入的 startTime。
        double startTime = 0;
        /// 相邻帧的时间间隔（秒）。
        double interval = 0;
        /// 基频（Hz）。清音帧的取值由 interpolateUnvoiced 决定。
        std::vector<float> f0;
        /// 与 f0 等长；1 表示浊音。
        std::vector<uint8_t> voiced;
    };

    /// 执行面由库实现（AnalysisTask，A15），提供者只实现 run()。停止后 start() 返回
    /// AnalysisError::Cancelled，不返回部分结果（A18、A28）。
    class Executive : public otter::AnalysisExecutive {
    public:
        using AsyncCallback =
            std::function<void(srt::Expected<std::unique_ptr<Result>>)>;

        srt::Expected<std::unique_ptr<Result>> start(const StartInput &input);
        srt::Expected<void> startAsync(std::shared_ptr<const StartInput> input,
                                       AsyncCallback callback);
        // state / stop / waitForFinished 转发给任务面；持有模型会话的提供者覆盖 stop / waitForFinished。

    protected:
        virtual srt::Expected<std::unique_ptr<Result>> run(const StartInput &input) = 0;
        bool cancelled() const noexcept;
    };

        /// 创建已载入模块的分析器；以 dynamic_cast 检查执行体属于本契约（A28）。
        static srt::Expected<std::unique_ptr<Executive>> create(srt::InferenceSpec &spec);

}
```

### 5.3 `org.openvpi.otter.inference.Note` Level 1

```cpp
namespace otter::Api::Note::L1 {

    inline const srt::ContribContract Contract{"org.openvpi.otter.inference.Note", 1};

    /// 转写得到的一个音符。
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

    /// 已知音符，转写以其为条件（对齐模式）。
    struct KnownNote {
        double start = 0;
        double duration = 0;
    };

    class Exports : public srt::ContribExports {
    public:
        int sampleRate = 0;
        int channelCount = 1;
        /// GAME 为 60。宿主切片时不得超过该值。
        double maxSegmentDuration = 0;

        /// 本模块支持的语言，ISO 639-3 代码。为空表示不区分语言。
        std::vector<std::string> languages;
        /// 调用方未指定语言时使用的语言。languages 非空时必填，且必须属于 languages（A27）。
        std::string defaultLanguage;
        /// 是否接受 knownNotes。
        bool supportsKnownNotes = false;

        /// 音符边界的判定阈值。
        Common::L1::Knob boundaryThreshold;
        /// 相邻边界的最小间隔（秒）。
        Common::L1::Knob boundaryRadius;
        /// 音符置信阈值，送入模型。
        Common::L1::Knob noteThreshold;
        /// 输出筛选阈值：置信度低于该值的音符不出现在结果中。
        Common::L1::Knob notePresenceCutoff;
        /// 迭代步数。
        Common::L1::IntKnob steps;

        /// 读取声明的 exports 块，与 F0 相同。game variant 的 configuration（五个模型路径、
        /// timestep、语言编号映射、扩散起点）是该 variant 自有的类型，宿主不读取。
        static srt::Expected<std::unique_ptr<Exports>> read(const srt::ContribSpec &spec);
    };

    class ImportOptions : public otter::AnalysisImportOptions { /* 同 F0 */ };
    class RuntimeOptions : public otter::AnalysisRuntimeOptions { /* 同 F0 */ };

    class StartInput : public srt::TaskStartInput {
    public:
        Common::L1::AudioSegment audio;
        Common::L1::ProgressCallback progress;

        /// 语言标识，必须属于模块声明的语言。未填时取模块默认值。
        std::optional<std::string> language;
        std::optional<double> boundaryThreshold;
        std::optional<double> boundaryRadius;
        std::optional<double> noteThreshold;
        std::optional<double> notePresenceCutoff;
        std::optional<int> steps;

        /// 已知音符，时间为宿主时间轴上的绝对秒（与 audio.startTime 同一基准，X3）。为空表示自由转写。
        std::vector<KnownNote> knownNotes;
    };

    class Result : public srt::TaskResult {
    public:
        /// 按起点升序排列，时间为绝对秒。
        std::vector<NoteInfo> notes;
    };

    class Executive : public otter::AnalysisExecutive {
        // 与 F0 同形：非虚的 start / startAsync，受保护的纯虚 run 与 cancelled；另有 create
    };

}
```

### 5.4 `org.openvpi.otter.inference.Align` Level 1

```cpp
namespace otter::Api::Align::L1 {

    inline const srt::ContribContract Contract{"org.openvpi.otter.inference.Align", 1};

    /// 某语言歌词的书写形式。
    enum class LyricsForm {
        Scheme,   ///< 以该语言 scheme 书写的音节（如拼音），即同一语言与 scheme 的 wolf G2P 的输出
        Text,     ///< 该语言的普通文字，由模块内部转换
    };

    /// 模块支持对齐的一门语言（A27）。
    struct LanguageInfo {
        std::string language;                ///< ISO 639-3 代码
        std::string scheme;                  ///< 结果中音素的 scheme，采用 wolf 的命名
        LyricsForm lyrics = LyricsForm::Scheme;
        std::vector<std::string> phonemes;   ///< 结果中可能出现的音素（不含静音与非语音标签）
    };

    /// 对齐得到的一个音素。
    struct PhoneInfo {
        std::string text;
        double start = 0;
        double duration = 0;
    };

    /// 对齐得到的一个词：来自调用方经 lyrics 给出的词，或模块自行插入的段
    /// （静音使用 silenceLabel，非语音使用 nonSpeechPhonemes 中的标签）。
    struct WordInfo {
        std::string text;
        double start = 0;
        double duration = 0;
        std::vector<PhoneInfo> phones;   ///< 按顺序排列，恰好覆盖该词
    };

    class Exports : public srt::ContribExports {
    public:
        int sampleRate = 0;
        int channelCount = 1;
        /// 单次执行接受的最长片段（秒）。对齐模型编码超长音频会直接失败，因此该值是硬约束。
        double maxSegmentDuration = 0;

        /// 支持对齐的语言。同一语言可以以不同 scheme 出现多次；同一 (language, scheme) 只能出现一次。
        std::vector<LanguageInfo> languages;
        /// 调用方未指定语言时使用的语言。languages 非空时必填，且必须属于 languages。
        std::string defaultLanguage;
        /// 模块能自行检测的非语音标签（变体自有的拼写，如气口）。
        std::vector<std::string> nonSpeechPhonemes;
        /// 调用方未指定时检测的非语音标签，为 nonSpeechPhonemes 的子集。
        std::vector<std::string> defaultNonSpeechPhonemes;
        /// 代表静音的词的标签。为空表示本模块不为静音命名，也不将静音作为词报告。
        std::string silenceLabel;

        Common::L1::Knob nonSpeechThreshold;    ///< 判定非语音的帧概率阈值
        Common::L1::Knob nonSpeechMinDuration;  ///< 报告的最短非语音段（秒）
        Common::L1::Knob gapFill;               ///< 短于该值（秒）的词间空隙并入相邻词，而非留作静音

        /// 读取声明的 exports 块。
        static srt::Expected<std::unique_ptr<Exports>> read(const srt::ContribSpec &spec);
    };

    class StartInput : public srt::TaskStartInput {
    public:
        Common::L1::AudioSegment audio;
        Common::L1::ProgressCallback progress;

        /// 语言标识，必须属于模块声明的语言。未填时取 defaultLanguage。
        std::optional<std::string> language;
        /// language 的 scheme。模块对该语言只声明一个 scheme 时可以不填；声明了多个时必填。
        std::optional<std::string> scheme;
        /// 所唱的文本，以空格分隔，写法由所用语言与 scheme 的 LanguageInfo::lyrics 规定。必填，为空即拒绝。
        std::string lyrics;
        /// 要检测的非语音标签。为空时取 defaultNonSpeechPhonemes。含模块未声明的标签即拒绝，不静默忽略。
        std::vector<std::string> nonSpeechPhonemes;

        std::optional<double> nonSpeechThreshold;
        std::optional<double> nonSpeechMinDuration;
        std::optional<double> gapFill;
    };

    class Result : public srt::TaskResult {
    public:
        /// 本次执行实际使用的语言与音素 scheme。
        std::string language;
        std::string scheme;
        /// 按起点升序排列，绝对秒。silenceLabel 非空时覆盖整段：首词起于音频起点，末词止于音频终点，
        /// 未归入任何词的空隙成为以 silenceLabel 为标签的词。silenceLabel 为空时，词可以不覆盖整段，
        /// 宿主按音频边界补齐。模块可以不支持空标签，此时它拒绝执行（加载期或执行期报 InvalidFormat），而不是返回词不覆盖整段的结果，宿主按该错误处理。
        std::vector<WordInfo> words;
    };

    class Executive : public otter::AnalysisExecutive {
        // 与 F0 / Note 同形：start / startAsync / state / stop / waitForFinished / run / cancelled
    };

}
```

Align 契约与另外两份契约在以下三处取舍不同：

+ **`lyrics` 为必填项。** 强制对齐的任务是在已知文本的前提下求时间，缺少文本则该次执行无意义。若将「未给歌词时按转写执行」设为默认分支，就相当于在同一份契约中并入另一份契约，即预留的 `Transcribe`。
+ **语言由 ISO 代码与附加信息共同描述。** 每门语言附带音素所用的 scheme、歌词写法（`scheme`：以该 scheme 书写的音节，即同一语言与 scheme 的 wolf G2P 的输出；`text`：普通文字，由模块自行转换）以及结果中可能出现的音素表（A27）。调用方可以另行指定 `scheme`，结果回写实际使用的 `language` 与 `scheme`。模型自身的语言代码不进入契约，其映射位于 `configuration.languages`（A11）。变体若声明了语言，每门语言都必须有词典，并在加载期校验（A21）。
+ **静音与非语音均作为词返回。** 契约只有 `WordInfo` 一种段。静音使用 `silenceLabel`，气口使用 `nonSpeechPhonemes` 中的标签，宿主按标签过滤即可，无须从空隙反推。`silenceLabel` 为空表示该模块不为静音命名，此时结果不保证覆盖整段；这一差异由声明体现，而非由结果体现。本仓的 `scripts/check-declarations.py`（`:451-453` 要求 Align 声明该标签、`:419-421` 要求它非空）在打包装配后运行（`scripts/make-package.py`），因此本仓出厂的 Align 包恒为非空，「空标签」这条路径只对跳过该 lint 的第三方变体有意义。

### 5.5 旋钮与三层的对应

spec 2.4 将声明文件分为三层，三类内容分别对应：

| 内容 | 落点 | 读取方 | 示例 |
| :-- | :-- | :-- | :-- |
| 宿主需要理解的能力与旋钮域 | `exports` → `Exports` | 宿主 | `sampleRate`、`maxSegmentDuration`、`boundaryThreshold` 的取值域与默认值 |
| 仅解释器关心的实现参数 | `configuration` → 变体的配置类型 | 解释器 | 模型路径、`timestep`、语言编号映射、扩散起点 |
| 每次执行的可调值 | `StartInput` | 调用方填写 | `boundaryThreshold = 0.3` |

判据沿用 spec 2.4 §《何时递增 Level》：**导入方是否需要理解该内容**。`timestep` 改变后模型无法正确运行，它描述模型本身而非可调参数，因此属于 `configuration`；`boundaryThreshold` 的松紧由用户直接感知，因此进入契约。

旋钮一律为 `std::optional`，未填时取模块默认值。将来增加旋钮时，不填写新旋钮的旧宿主行为不变（A9）。

## 6. 模型包结构

抽参模型按 spec 2.4 打包为 Package，与声库包共用同一套 `SynthUnit` 与搜索路径（A13）。

```
+ otter-rmvpe
  + inferences
    + f0
      - inference.json
  - desc.json
  - rmvpe.onnx          # 图，约 345 MB
```

模型位于包根，声明从 `inferences/f0/` 引用模型。规范 2.4 的相对路径以**声明文件自身所在的目录**为基准，因此写作 `../../rmvpe.onnx`；写作 `./rmvpe.onnx` 则指向声明文件所在的目录。

`desc.json`：

```json
{
  "$version": "1.0",
  "id": "otter/rmvpe",
  "version": "0.1.0.0",
  "compatVersion": "0.1.0.0",
  "runtimeLevel": 1,
  "contributions": {
    "inference": [
      { "id": "f0", "path": "./inferences/f0/inference.json" }
    ]
  }
}
```

`inference.json`：

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

GAME 的结构相同：`contributions.inference` 含一项，`interface` 为 `org.openvpi.otter.inference.Note`， `exports` 另含 `languages`（ISO 639-3 列表）、`defaultLanguage`、`supportsKnownNotes` 与五个旋钮， `configuration` 列出四到五个 session 的路径（`durationToBoundary` 可选）、`timestep`、语言标识到模型编号的映射，以及可选的 `scheduleStart`。三个契约的 `exports` 与 `imports[].options` 各有一份 JSON Schema，位于 `docs/schemas/`。

HFA 是第一个将模型自带文件一并打包的变体（A21）：图、前端配置、词表与三本词典同置于包根，由 `inference.json` 的 `configuration` 引用。

```
+ otter-hfa
  + inferences
    + align
      - inference.json
  - desc.json
  - model.onnx          # 图，约 415 MB
  - config.json         # mel_spec_config：44100 Hz、hop 441
  - vocab.json          # 音素类、非语音类、词典名表
  - ds-zh-pinyin-lite.txt
  - ds_cmudict-07b.txt
  - japanese_dict_full.txt
```

```
  "configuration": {
    "model": "../../model.onnx",
    "config": "../../config.json",
    "vocab": "../../vocab.json",
    "languages": { "cmn": "zh", "eng": "en", "jpn": "ja" }
  }
```

TIFA 是第二个将模型自带文件一并打包的变体（A29）：五张图（合计约 160 MB）与导出器写出的 `config.json`、`vocabulary.json` 由上游产出，模型的四本「书写单位 → 音素」词典随包安装，`inference.json` 的 `configuration` 以十一处路径与一张语言映射指向它们。这十一处模型文件与声明都不入库；装配时用 `scripts/make-package.py --variant tifa --declarations <声明目录> --models <模型目录>` 把模型复制到声明写的位置。

```
+ otter-tifa
  + inferences
    + align
      - inference.json
  + dictionaries
    - ds-zh-pinyin-lite.txt      # cmn
    - jyutping_dict.txt          # yue
    - japanese_dict_full.txt     # jpn
    - ds_cmudict-07b.txt         # eng
  - desc.json
  - spectrogram.onnx             # 音频前端：log-mel 与 maskT
  - model.onnx                   # 相似度与 token logits（进图两次）
  - prepare.onnx                 # 打分模板
  - score.onnx                   # 片段代价
  - select.onnx                  # 按候选选择选路径
  - config.json                  # 导出器写的：48000 Hz、hop 480、fft/win 2048、mel 80
  - vocabulary.json              # 符号 → id
```

```
  "configuration": {
    "spectrogram": "../../spectrogram.onnx",
    "model": "../../model.onnx",
    "prepare": "../../prepare.onnx",
    "score": "../../score.onnx",
    "select": "../../select.onnx",
    "config": "../../config.json",
    "vocabulary": "../../vocabulary.json",
    "dictionaryCmn": "../../dictionaries/ds-zh-pinyin-lite.txt",
    "dictionaryYue": "../../dictionaries/jyutping_dict.txt",
    "dictionaryJpn": "../../dictionaries/japanese_dict_full.txt",
    "dictionaryEng": "../../dictionaries/ds_cmudict-07b.txt",
    "languages": { "cmn": "zh", "yue": "yue", "jpn": "ja", "eng": "en" }
  }
```

四个变体的声明不入库（一个变体一个目录），版本均为 0.1.0.0（`inferences/` 布局， A26）；模型文件同样不入库，随各自的包发布。装配由 `scripts/make-package.py` 完成，声明目录由 `--declarations` 给出（缺省指仓根下的 `packages/`，该目录已移出 git 跟踪）。库版本、包版本与 `manifest.json` 的 `bundleVersion` 同源，唯一权威是 `CMakeLists.txt` 的 `project(... VERSION ...)`：脚本从那里读，声明 lint 用它核对声明（A31）。发布标签 `models-v0.1` 提供前述四个包：一份列出四者的 `manifest.json` 与由该脚本生成的四个 zip。

## 7. 插件结构

按 spec 2.4 §3，插件嵌入 IID `org.openvpi.synthrt.plugin.InferenceInterpreter`，同目录下有 `plugin.json`：

```json
{
  "name": "otterrmvpe",
  "interpreters": [
    { "interface": "org.openvpi.otter.inference.F0", "level": 1, "variant": "rmvpe" }
  ]
}
```

`game` 与两个对齐变体结构相同（`ottergame` 对应 `org.openvpi.otter.inference.Note`，`otterhfa` 与 `ottertifa` 对应 `org.openvpi.otter.inference.Align`）。测试桩以 variant `stub` 同时声明三份契约，它是测试里的静态插件，不是插件目录（A33）。

插件的 CMake 目标由 `otter_add_interpreter_plugin(<target> <目录>)` 声明（A28），它以 `otter_add_plugin()` 声明插件，再组合 synthrt 的 `AddPlugin.cmake` 提供的 `synthrt_add_plugin_objects()`、`synthrt_configure_plugin_manifest()` 与 stdcorelib-plugin 的 `stdc_add_plugin_metadata()`。源文件是插件目录下的全部头文件与源文件，目录名即 include 命名空间（`<tifa/Decode.h>`）。`main.cpp` 只放插件类，其余源文件编成 OBJECT 库 `<target>_objects`，由插件与测试共用。解释器类声明在 `<X>Interpreter.h`，成员函数的定义连同配置、执行体与解释器原有的私有静态辅助函数放在同名 `.cpp` 的匿名命名空间里。命名空间为 `otter::<插件>`，`game-ggml` 与 `tifa-ggml` 为 `otter::gameGgml` 与 `otter::tifaGgml`。`otterhfa` 另有 `Aligner`、`G2p`、`NonSpeech` 三对源文件，`ottertifa` 另有 `Decode`、`Grid`、`Scoring`。插件的头文件只用到 otter 与 synthrt，所以只有 `synthrt-otter` 是公开依赖，`analysisutil`、`onnxutil`（或 `ggmlutil`）与 `synthrt::dsinfer` 都是私有依赖。插件拒绝自己不服务的三元组时用 `checkServed` 的 `FeatureNotSupported`。各插件不再各有一份 CMakeLists，声明集中在 `inferenceinterpreters/CMakeLists.txt`。

宿主将 otter 的 `inferenceinterpreters` 目录加入 inference 类别的插件搜索路径：`SynthUnit::setPluginPaths(srt::InferenceCategory::NAME, ...)` 的序列中列出 `prefix / "lib/synthrt-otter/plugins/inferenceinterpreters"`，与 dsinfer 和 wolf 的目录并列。

## 8. 宿主责任

otter 将以下三项职责明确留给宿主，三者缺一不可：

1. **重采样。** 宿主读取 `Exports::sampleRate` / `channelCount`，将音频转换为该格式。RMVPE 要求 16000 Hz 单声道，GAME 与 HFA 要求 44100 Hz 单声道，TIFA 要求 48000 Hz 单声道。各算法要求不同，宿主必须采用模块声明的值，不得写死。执行体校验输入，不匹配即失败，不会自行重采样。
2. **切片。** 宿主以 RMS 静音切片将音频分段，每段不超过 `Exports::maxSegmentDuration`，逐段调用，并为每段正确填写 `audio.startTime`。otter 不切片（A7）。
3. **时间轴换算。** otter 输出绝对秒，宿主按自身的时间轴换算为 tick，变速曲由宿主处理。

在 lite 中，重采样由 talcs 完成；RMS 切片器取自 lite 历史中已删除的 `src/libs/audio-util`（该目录与 `game-infer`、`rmvpe-infer` 在同一次提交中被删除），现位于 `Modules/Extractors/AudioSlicer.h`，详见 [LiteIntegration.md](LiteIntegration.md)。三个算法共用一套切片参数（A8）。

## 9. 决策台账

每条决策记录决策内容、依据与代价。已被取代的决策保留原文，并在标题下注明取代它的决策；现状以取代方为准。

### A1 — otter 注册自有的 `analysis` 类别，不复用 `inference`

> **已被 A26 取代。** 本条的前提与实现不符，otter 现不注册自有类别。以下内容仅作历史记录。

**决策**：otter 注册新贡献类别 `analysis`，并定义自有的 `ContribSpecExtension` 作为顶层执行体入口。

**依据**：`ContribSpec` 无公开的执行体入口；`ContribCategory::createExecutiveFactory()` 只接受 `ContribImportBinding`（`synthrt/include/synthrt/Core/ContribCategory.h`）；框架唯一的顶层入口是 `SingerPipelineExtension`（`SingerPipelineExecutive.h`）。抽参器不被任何模块 import，因此只能自建入口；而扩展点的 traits 以 spec 类型为键，因此需要自有的 spec 类型，也就需要自有的类别。

**备选与否决理由**：
- *将抽参器实现为 `inference` 贡献*：当时认为 `InferenceSpec` 同样只能经 import 创建执行体，宿主无法获得顶层入口。否决。（A26 表明这一判断有误。）
- *让抽参模块 import 一个 `inference` 模块（wolf linguist 的做法）*：wolf 需要该结构，是因为 linguist 是 G2P/S2P/Onset 的**组合**；抽参器只是单个模型，多一层间接只会增加包结构的复杂度。且 A3 表明获取驱动不需要这一层。否决。

**代价（已接受）**：宿主需要多配置一条 `analysis` 的插件搜索路径。

### A2 — `analysis` 类别不追加字段

> **已被 A26 取代。** `analysis` 类别已删除；「不追加字段」的结论对 inference 类别同样成立（第 4.3 节）。

**决策**：类别层不追加任何字段，只使用 spec 2.4 的公共字段。

**依据**：类别追加字段的意义在于「在选定解释器之前就需要读到」（spec 2.4《模块》三层表）。宿主在设置界面列出已安装抽参器时需要的是 `name` / `interface` / `variant`，均为公共字段。

**代价（已接受）**：宿主无法在 `DataOnly` 模式下读取采样率等能力，这些能力位于 `exports` 中，需要 `Load` 之后才可读取。抽参器数量少，宿主本就全部装载，因此该代价可以接受。

### A3 — 直接获取 `InferenceDriver`，不经 `inference` 模块中转

**决策**：解释器经 `spec.package().synthUnit().runtimeService(ds::InferenceDriverPlugin::IID, "onnx")` 获取 ONNX 驱动。

**依据**：`ds::InferenceDriver` 派生自 `srt::RuntimeService`（`dsinfer/include/dsinfer/Inference/InferenceDriver.h`），与类别无关；wolf 的 `multig2p-onnx` 插件已采用此做法。refactor 的 `srt::extract::getInferenceDriver(runtime)` 属于 refactor 的抽参子系统，随 `srt-core` 一同丢弃。

### A4 — 一个类别容纳 F0 / Note / Align / Transcribe

> **已被 A26 取代。** 四份契约现同属 synthrt 内置的 `inference` 类别，仍以 `interface` 区分。

**决策**：四份契约同属 `analysis` 类别，以 `interface` 区分。

**依据**：spec 2.4 规定「输入输出已根本不同时另起一份 `interface`」，而非另起一个类别；类别的扩展点用于「需要一种全新的模块」。四者都由模块声明、解释器与顶层执行体构成，结构相同。

**代价（已接受）**：宿主枚举 `analysis` 贡献时需要按 `interface` 筛选。

### A5 — 时间一律使用绝对秒（`double`）

**决策**：所有对外时间量均为 `double` 秒，包括 `NoteInfo::start` / `duration`、`Result::interval` / `startTime`、`AudioSegment::startTime`、`boundaryRadius`。契约中不出现 tick，也不出现毫秒。

**依据**：dsinfer 的 L1 契约全部采用秒（`CommonApiL1.h` 中的 `InputNoteInfo::duration`、 `InputParameterInfo::interval`、`InputPhonemeInfo::start`）。同一宿主中并存两套时间单位会造成长期负担。此外模型本身产出秒（GAME 的 `durations` 以秒为单位），整数化会引入累加漂移，refactor 的 tick 路径正是因此产生偏移。

**备选与否决理由**：
- *整数毫秒*：便于跨语言绑定传递，但逐音符累加时 1 ms 量化在长句上产生漂移，且与 dsinfer 契约口径不一致。否决。
- *音符用毫秒、f0 用秒*：同一套契约使用两种单位，增加使用方的记忆负担。否决。

### A6 — 清浊标志命名为 `voiced`，1 表示浊音

**决策**：字段名为 `voiced`，类型为 `uint8_t`，1 表示浊音。

**依据**：refactor 的字段名为 `uv`（unvoiced），注释却写「true=浊音 voiced」，名实相反，是明确的缺陷诱因。`std::vector<bool>` 同时改为 `std::vector<uint8_t>`：前者的位压缩特化不能取元素地址，也不能安全地跨线程按元素写入。

### A7 — 切片归宿主，otter 不切片

**决策**：执行体只接受一段连续音频，不做静音切片。宿主负责分段并逐段调用。`Exports` 声明 `maxSegmentDuration` 作为硬约束。

**依据**：该分工由需求方规定。切片所需的信息（编辑区域、可见范围、用户选区）由宿主掌握，宿主的切片更准确；otter 也因此完全不需要音频侧代码。

**代价（已接受）**：`Exports` 不声明切片参数。若将来某个算法确实需要不同的切片参数，将其加入 `exports` 词汇需要递增 Level。本方案接受该代价，不为当前明确不读取这些参数的宿主预留词汇。

### A8 — 各算法共用一套切片参数

**决策**：宿主使用一套切片配置，不按算法区分。

**依据**：换算为时间后，两套参数几乎相同：`hopSize` 均为 10 ms（16 kHz 下 160、44.1 kHz 下 441）， `winSize` 均为 40 ms，`minInterval` 均为 300 ms，`maxSilKept` 均为 500 ms，阈值均为 0.02；仅 `minLength` 不同（RMVPE 5 s，GAME 2 s）。因此采用一套参数，GAME 与 HFA 的 60 s 上限由宿主保证不被超过。

### A9 — 旋钮一律为 `std::optional`，缺省取模块默认值

**决策**：`StartInput` 中每个旋钮为 `std::optional<T>`；未填写时解释器采用声明 `exports.knobs` 中的默认值（A17 起默认值与取值域一同归契约）。`Exports` 同时声明本模块是否采用该旋钮、取值域与默认值；对于模块未采用的旋钮，调用方给出的值被忽略。

**依据**：与 dsinfer 的 `AcousticStartInput::depth` 加 `AcousticConfiguration::useVariableDepth` 做法相同。宿主可以依据 `Exports` 自动生成设置界面；新算法增加旋钮时，不填写该旋钮的宿主行为不变。

**备选与否决理由**：
- *开放的 JSON 字典*：新算法增加参数时宿主无须修改，但完全丧失编译期类型安全，且不同算法可能以不同键名表示同一参数，契约因此失去约束力。否决。
- *变体私有参数一律固定在 `configuration` 中，用户不可调*：契约最小、最稳定，但用户调整阈值需要重新发布模型包，与「参数尽量可调」的需求冲突。否决。

### A10 — 进入契约的旋钮清单

**决策**：

| 契约 | 旋钮 | refactor 中对应的硬编码 |
| :-- | :-- | :-- |
| F0 | `voicingThreshold` | RMVPE `RmvpeExtractor.cpp` 中的 `0.03f` |
| F0 | `interpolateUnvoiced` | RMVPE 无条件调用的 `interpF0()` |
| Note | `boundaryThreshold` | `seg_threshold`，送入 segmenter 的 `threshold` |
| Note | `boundaryRadius` | `seg_radius_seconds`，除以 `timestep` 得到 `radius` 帧数 |
| Note | `noteThreshold` | `est_threshold`，送入 estimator 的 `threshold` |
| Note | `notePresenceCutoff` | `buildMidiNotes` 中的 `presence > 0.5f` |
| Note | `steps` | `generateD3pmTs()` 中写死的 `n_steps = 8` |
| Note | `language` | `config.json` 的 `languages`（改为字符串标识） |
| Note | `knownNotes` | segmenter 的 `known_boundaries` / `prev_boundaries` |

**依据**：清单覆盖两个插件送入 ONNX session 的全部输入（segmenter：`x_seg`、`maskT`、 `known_boundaries`、`prev_boundaries`、`language`、`threshold`、`radius`、`t`；estimator： `x_est`、`boundaries`、`maskT`、`maskN`、`threshold`；RMVPE：`waveform`、`threshold`）。`t`（扩散时间步）由 `steps` 与 `configuration` 的 `scheduleStart` 生成，不整表公开。

**实现时的修正**：`t` 属于**批次维**而非步数维。真实导出模型的每个 segmenter 输入共享同一个批次维， `t` 也不例外，因此一次调用只携带一个时间步，**采样循环由调用侧实现**，每一步将上一步的输出作为 `prev_boundaries` 送回。三个阈值类输入（segmenter 的 `threshold` / `radius`、estimator 的 `threshold`）是**标量**，秩不符时被拒绝而不是广播。estimator 的 `presence` 输出为**布尔**张量，按张量自身的元素类型读为取值 1 或 0 的置信度，因此 `notePresenceCutoff` 的 0.5 仍能区分两类。

**保留在 `configuration` 中的内容**：`timestep`、`scheduleStart`、语言编号映射、各 session 路径。这些内容改变后模型无法正确运行，描述的是模型本身。`sampleRate` 与旋钮默认值按 A17 归 `exports`。

### A11 — `language` 使用字符串标识，映射写在 `configuration`

**决策**：契约中的 `language` 是字符串标识，按 A27 为 ISO 639-3 代码（如 `"cmn"`、`"jpn"`）；模型内部的整数编号由 game 变体 `configuration` 的 `languages` 映射。

**依据**：整数编号是单个模型的内部约定，两个模型的编号 `1` 未必表示同一种语言，而契约词汇必须跨变体稳定。dsinfer 的 `AcousticConfiguration::languages`（`std::map<std::string, int>`）采用相同做法。

### A12 — 对齐模式经 `knownNotes` 表达，补回 `dur2bd` session

**决策**：`StartInput::knownNotes` 非空即为对齐模式；解释器经 `dur2bd` 将已知时长转换为边界，送入 segmenter 的 `known_boundaries`。`Exports::supportsKnownNotes` 声明本模块是否接受已知音符。

**依据**：该能力由模型提供：旧 `GameModel` 包含 `runDur2bd` 与 `InferenceInput::known_durations`； refactor 的 `GameExtractor` 迁移时遗漏了该 session，`inferSlice` 中的 `knownBoundaries` 恒为全零。移植时按旧实现补回。

**代价（已接受）**：ASR 与音素级强制对齐不经此路径实现，二者分别属于独立的 `interface`：强制对齐已由 A19 立为 `Align` 契约，ASR 为预留的 `Transcribe`。

### A13 — 抽参模型包与声库包共用同一 `SynthUnit`

**决策**：宿主使用同一个 `SynthUnit`、同一套 Package 搜索路径装载两类包，按类别与接口区分。

**依据**：spec 2.4 的 Package 体系本身是统一的；使用两个 unit 需要维护两套生命周期，且抽参模型将来可能被声库包的 `dependencies` 引用（例如某声库自带专用的音高模型）。

**代价（已接受）**：`SynthUnit::loadedPackages()` 同时包含两类包，宿主枚举时需要按贡献的类别与接口筛选。

### A14 — 提供者不得将扩展挂载到其不服务的声明上

> **已被 A26 取代。** 扩展机制与 `AnalysisProvider` 已随 `analysis` 类别一并删除。以下内容仅作历史记录。

**决策**：`AnalysisProvider` 的构造函数接收其服务的 (interface, level, variant)，`createExtensions` 声明为 `final`，先以 `serves()` 检查，再委托给 `createAnalysisExtension()`。

**依据**：`PackageLoader` 在挂载 spec 扩展时（`synthrt/lib/Core/PackageLoader.cpp` 中的 `attachSpecExtensions`）将**每一个已注册解释器**的 `createExtensions` 作用于**每一份正在加载的声明**。这是 spec 2.4 的有意设计，使其他类别可以为新 role 提供执行体。无条件接受的提供者会将扩展挂满整个包，宿主向某份声明索取其不具备的契约时，得到的是一个不属于该声明的执行体，而不是 nullptr。

**代价（已接受）**：提供者不能自行决定扩展的挂载条件。确有需要时可以改为受保护的钩子，但须等到出现实际需求。

**发现途径**：当时的 `test_AnalysisLoad` 用例断言 f0 声明上不存在 Note 扩展；两个提供者当时均将扩展挂满整个包。

### A15 — 取消的归属在调用方线程上确定

**决策**：执行的认领与上一次取消标志的清除在调用方线程上完成；工作线程只执行函数体，不清除标志。同步与异步两个入口均先完成认领。

**依据**：若在工作线程开始执行时清除标志，异步入口会出错：`stop()` 可能在 `startAsync()` 返回之后、工作线程进入函数体之前到达，该次取消会被清除。测试中该情形可稳定复现。

**代价（已接受）**：增加一个类型。否则三个执行体须各自实现同一段并发代码，而该代码的错误不会产生任何可见症状。

**补记（2026-11-18）**：claim、generation、同步/异步互斥、callback 交付、异常捕获和等待屏障已经上移到 synthrt 的统一 `srt::ITask` 内核。`otter::AnalysisTaskBase` 不再保存第二套 running/cancellation 状态，只检查输入类型、把取消错误分类为 `Canceled`，并通过错误映射钩子保留 `AnalysisError::Internal` 与 `NoWorker`。模板 `otter::AnalysisTask<Input, Result>` 负责 typed body、结果转换和 typed callback。同步与异步入口共用同一个 claim，空闲 `stop()` 不预取消下一次执行，callback 可以启动下一次执行或销毁分析器。完整协议见 `synthrt/docs/codex/TaskLifecycle.md`。

### A16 — 进度回调位于 `StartInput`

**决策**：`ProgressCallback` 是 `StartInput` 的字段，而非 `RuntimeOptions` 的字段。

**依据**：进度是单次执行的属性，而非执行体的属性：同一执行体复用于多段音频时，每段的进度接收方可能不同。synthrt 主线没有进度回调的先例，此处为新增；置于单次调用的输入中代价最小。

### A17 — `exports` 归契约，`configuration` 归变体，接口名带项目段

> **接口名与扩展 ID 部分已被 A26 取代。** 接口名现为 `org.openvpi.otter.inference.*`，扩展已删除； `exports` 与 `configuration` 的分工仍然有效。

**决策**：采样率、声道数、帧间隔、段上限、语言标识、`supportsKnownNotes` 与全部旋钮域写在声明的 `exports` 中，由库中每份契约各一个的读取函数（`F0::L1::Exports::read` / `Note::L1::Exports::read` / `Align::L1::Exports::read`）解析，并发布 JSON Schema（`docs/schemas/`）。`configuration` 只包含变体私有内容：rmvpe 的模型路径； game 的五个模型路径、`timestep`、语言编号映射与 `scheduleStart`；hfa 的模型、mel 配置、词表的路径与语言编号映射（词典由词表定位，见 A21）。变体在加载期将两块内容相互核对：模型无法支持的采样率、声明了但缺少模型的对齐路径、词表中缺少对应词典的语言，均在加载期拒绝。接口名当时改为 `org.openvpi.otter.analysis.F0` / `.Note` / `.Align`，扩展 ID 改为 `org.openvpi.otter.extension.*`。

**依据**：spec 2.4 §「三个语法块的归属」规定，`exports` 语法归 `interface + level`、由导入方读取， `configuration` 全部归 `variant`；「需要参与跨模块契约的内容应由 `exports` 公开，而不是作为 `configuration` 中的契约字段」。若将契约事实放入各变体的 `configuration` 再派生 `exports`，每增加一个变体就要为同一批事实另立一套键，`DataOnly` 模式读不到能力，文档示例也与 lint 规则相反。`hfa` 作为第三个变体验证了这一分工：其 `configuration` 只含私有的文件路径与语言映射，没有重复任何契约事实。接口名采用与 `org.openvpi.dsinfer.inference.*`、`org.openvpi.wolf.inference.*` 一致的项目段写法，并在尚无已发布包时完成改名。

**取代**：`F0Configuration` / `NoteConfiguration` 两个接口头中的配置类型（改由变体各自定义），以及「声明不得写 exports」的 lint 规则。

### A18 — 取消与工作线程失败使用 otter 自有的错误类别

**决策**：新增 `otter::AnalysisError` 及其 `std::error_category`（`otter/Analysis/AnalysisError.h`），初始取值为 `Cancelled` 与 `NoWorker`（A28 增加 `Internal` 与 `ModelFailed`，A30 拆分为 `Internal` / `ContractViolation` / `ModelFailed` / `ModelMismatch`）。被 `stop()` 中止的执行以 `AnalysisError::Cancelled` 返回错误，`state()` 为 `Canceled`；`startAsync()` 无法创建工作线程时返回 `NoWorker`。

**依据**：synthrt 的 `Error.h` 规定框架的错误码枚举不由上层库扩展，上层库应注册自有的类别。此前三个提供者以 `InvalidArgument` 表示取消，宿主只能依靠 `state()` 区分取消与失败；且 spec 2.4 要求契约规定其错误语义。契约的错误条件因此为本类别的取值加上框架的 `InvalidArgument` / `InvalidFormat` / `FeatureNotSupported`。

**代价（已接受）**：增加一个头文件与一个 `.cpp`；宿主比较 `code()` 时需要识别一个额外的类别。

### A19 — 强制对齐立为独立契约 `Align`，不并入 F0 或 Note

**决策**：新增 Align Level 1 契约（`include/otter/Api/Align/1/AlignApiL1.h`、 `src/lib/Api/Align/1/AlignApiL1.cpp`、`docs/schemas/align-1-exports.schema.json`），由 `hfa` 变体实现。接口名当时为 `org.openvpi.otter.analysis.Align`，按 A26 现为 `org.openvpi.otter.inference.Align`。 `Align` 自此从预留名单移入 `scripts/check-declarations.py` 的 `IMPLEMENTED_INTERFACES`，该脚本增加了 Align 声明的校验规则。

**依据**：Align 的输入与输出均不同于另两份契约：输入多出一段必填的文本（所唱内容），输出是词与音素两层区间，而非曲线或音高。并入 F0 相当于将词的时间视为另一条曲线，并入 Note 相当于将音素视为音符，两种方式都会使宿主按契约名读到结构不符的结果。该契约名已在 A4 中登记，本条给出其定义。

**来源**：hfa 方案（[plans/HfaAlign.md](plans/HfaAlign.md)）的决策 D1，经用户确认。

### A20 — `Align` 的输入是歌词文本，缺少歌词时不退化为转写

**决策**：`StartInput::lyrics` 为必填的单个字符串，为空即拒绝。文本写法由变体规定（`hfa` 要求其词典可读的写法：中文为拼音音节、英文为单词、日文为罗马字，以空格分隔；A27 起写法由 `LanguageInfo::lyrics` 声明），契约只规定这是一段传给变体的文本。

**依据**：强制对齐是在已知文本的前提下求时间，缺少文本则该次执行无意义。将其降级为「未给歌词时按 ASR 执行」相当于在同一份契约中再并入一份契约。文本写法归变体，是因为不同对齐器的词典所接受的写法本就不同，而宿主只需将其作为文本转发。

**来源**：hfa 方案的决策 D4，经用户确认。

### A21 — 模型自带的文件随包发布，按 `configuration` 读取并在加载期相互核对

**决策**：`hfa` 包将模型的 `config.json`（前端配置：采样率与帧移）、`vocab.json`（音素类、非语音类、词典名表）、三本词典与 `model.onnx` 一并发布；`configuration` 以 `model` / `config` / `vocab` 三个路径指向这些文件，以 `languages` 将宿主语言标识映射到模型自身的语言代码（两项默认值此后移入 exports，见 A27）。加载期核对以下各项：声明的采样率等于 `config.json` 的 `mel_spec_config.sample_rate`；每个声明语言在 `vocab.json.dictionaries` 中有条目，对应文件确实存在于词典位置，且词表中存在带该语言前缀的音素； `nonSpeechPhonemes` 属于 `vocab.json.non_lexical_phonemes`；`silenceLabel` 属于 `vocab.json.silent_phonemes`；`defaultNonSpeechPhonemes` 属于 `nonSpeechPhonemes`。

**依据**：按 A17 的分工，能力进入 `exports`，模型的位置与接入方式进入 `configuration`。这些文件描述的是模型自身的事实，而非包声明的事实，因此由变体读取，不发布为 schema。但它们与声明之间的每一处矛盾都会表现为「可以运行但结果错误」（采样率错误导致整体速度错误，词典缺失导致整段没有词），因此必须在加载期拒绝。`scripts/check-declarations.py` 对同一批事实执行同样的静态校验，使不合格的包在入库前即被拦下。

**来源**：hfa 方案的决策 D1、D3，经用户确认。

### A22 — 静音与非语音段一律作为词返回

**决策**：`Result` 只有 `WordInfo` 一种段。模块检测到的静音以 `exports.silenceLabel` 为标签、非语音以 `exports.nonSpeechPhonemes` 中的标签为标签，作为没有对应歌词的 `WordInfo` 返回。`silenceLabel` 声明为空表示该模块不为静音命名，此时结果不保证覆盖整段。

**依据**：宿主需要「该时段属于何种内容」这一层信息。若由宿主从词的间隙反推，就相当于丢弃对齐器的分类结果，再由每个宿主重新计算。非语音类别的集合是模型的事实，因此以声明中的标签公开，而不在契约中枚举。

**来源**：hfa 方案的决策 D4，经用户确认。

### A23 — 三个硬编码常量升为旋钮，其余保留在实现中

**决策**：`nonSpeechThreshold`（参考实现中为 0.5）、`nonSpeechMinDuration`（0.1 s）、`gapFill`（0.1 s）写入 `exports.knobs` 并进入 `StartInput`。参考实现中另外两个常数，即非语音扫描允许的最大中断 5 帧与 10 帧的最短段，保留在实现中，按帧长换算，不公开。

**依据**：A10 的判据为「调用方是否直接感知其效果」。前三个常量在参考实现中即为用户可见参数（气口检测的松紧、空隙是否并入词），后两个是同一次判定的内部形状，公开后只会增加两个不会被调整的旋钮。

**来源**：hfa 方案的决策 D1，经用户确认。

### A24 — 不做解码、重采样与自动切段，超限即拒绝

**决策**：`hfa` 不读文件、不解码、不重采样（A7 的规则），也不实现参考实现中「超过 60 秒自动切段后逐段拼接」的逻辑。`maxSegmentDuration` 声明为 60，超限由 `otter::PreparedSamples::prepare()` 拒绝。

**依据**：切段跨越契约边界（每段的结果需要各自回到宿主的时间轴），自动切段出错的后果是不可见的边界错误；参考实现的该路径还包含一个 Python 版本的均值补齐变体，otter 中没有对应实现。宿主本就负责切片（A7），声明清楚上限即可。

**来源**：hfa 方案的决策 D1、D3，经用户确认。

### A25 — 模型包一次发布三份，`manifest.json` 全量重写

> **发布内容已由 A26 与 A31 更新。** 本条所述的 0.1.0.0 包采用旧布局，当前 otter 不再能够载入；A26 之后的 0.2.0.0 包曾由 `models-v0.3.0.0` 提供（历史，该标签已删除），A31 起包版本回到 0.1.0.0，现行发布为 `models-v0.1`（四个包与一份列出四者的 `manifest.json`）。本条的发布原则（清单全量重写、新版本另开标签）仍然有效。

**决策**（历史）：发布标签 `models-v0.2.0.0`，内含 `otter-rmvpe-0.1.0.0.zip`、`otter-game-0.1.0.0.zip`、 `otter-hfa-0.1.0.0.zip` 与一份列出三者的 `manifest.json`；前两份与 `models-v0.1.0.0` 中的文件逐字节相同（经 SHA512 核对后重新上传），第三份为新增。`hfa` 的权重取自 [HubertFA v0.0.7](https://github.com/wolfgitpr/HubertFA)（Apache-2.0），包内不附 `LICENSE`，来源与许可在 release notes 中说明。

**依据**：`manifest.json` 是宿主发现包的入口，只列出新包会使 `models-v0.1.0.0` 的既有用户失去前两个包；而同一标签下的资产不可修改，因此另开新标签。权重的许可与再分发条件属于事实问题，应在发布说明中写明，而不是不加说明地放入包内。

**来源**：hfa 方案的决策 D2、D3，经用户确认。

### A26 — 分析器作为 inference 模块，otter 不注册自有类别

**决策**：撤回 A1、A2、A4、A14。F0 / Note / Align 是 synthrt 内置 `inference` 类别下的模块，接口名为 `org.openvpi.otter.inference.{F0,Note,Align}`；解释器派生自 `srt::InferenceInterpreter`，插件嵌入 `srt::InferenceInterpreterPlugin::IID`，安装在 `lib/synthrt-otter/plugins/inferenceinterpreters`；执行体派生自 `srt::InferenceExecutive`。宿主通过契约执行体中的 `Executive::create(spec)` 在顶层创建分析器，该函数调用公开的 `srt::InferenceSpec::createInference`。

**依据**：A1 的前提「`InferenceSpec` 只能经 import 创建执行体」与 synthrt 的实现不符。 `InferenceSpec::createInference` 是公开方法，只校验模块已载入、选项与模块的契约一致，不要求存在 importer；wolf 的测试即以此方式在顶层创建 G2P/S2P 执行体。A1 的另一项理由「扩展 traits 以 spec 类型为键，因此需要自有的 spec 类型」也随之失效，因为不再使用扩展。自有类别不追加字段（A2 的结论对 inference 同样成立），其唯一作用是为扩展提供作为键的 spec 类型。

**得失**：删除类别层（`AnalysisContrib`、`AnalysisExtension`、`AnalysisProviderPlugin`、 `linkAnalysisCategory()` 与静态注册、`checkRuntimeOptions`）后，宿主无须为链接器保留锚点，也无须另配一条类别路径；包只依赖 Runtime Level 1 的内置类别，结构与 wolf 的 G2P/S2P 相同。分析器也因此可以被其他模块 import。`test_AnalysisContrib` 断言链接 otter 不注册任何类别。代价是接口改名、包布局从 `analyzers/*/analysis.json` 改为 `inferences/*/inference.json`，当时三个包升级到 0.2.0.0 重新发布（历史），旧布局包在当前 otter 下不再能够载入。发布标签 `models-v0.3.0.0` 曾提供这三个 0.2.0.0 包（一份 `manifest.json` 与由 `scripts/make-package.py` 生成的三个 zip），`models-v0.1.0.0`、`models-v0.2.0.0` 中均为 0.1.0.0 的旧布局包，三个标签此后都已删除；A31 起包版本回到 0.1.0.0，现行发布为 `models-v0.1`，提供四个 0.1.0.0 的包与一份列出四者的 `manifest.json`。

### A27 — 语言代码的 ISO 639-3 校验、对齐语言的 scheme 与音素表、默认值的契约归属

**决策**：
- Note 与 Align 的语言一律为 ISO 639-3 代码（`[a-z]{3}`），由契约的读取函数、linter 与 JSON Schema 三处强制校验。
- Align 的 `languages` 每项为 `{language, scheme, lyrics, phonemes}`：`scheme` 采用 wolf 的命名与语法，表示结果中音素的写法；`lyrics` 取 `scheme` 或 `text`，规定宿主提供歌词的写法；`phonemes` 为结果中可能出现的音素。同一 `(language, scheme)` 只能出现一次。`StartInput` 增加可选的 `scheme`，`Result` 回写 `language` 与 `scheme`。hfa 在加载期将 `phonemes` 与 `vocab.json` 中带该语言前缀的音素逐一比对，存在任何多余或缺失的音素即拒绝载入。
- `defaultLanguage`（Note、Align）与 `defaultNonSpeechPhonemes`（Align）从 configuration 移至 exports。列出语言时必须声明默认语言，且默认语言必须属于所列语言。该规则由 GAME 的变体规则上升为契约规则。

**依据**：
- 仅有 ISO 代码时，宿主无法确定歌词应采用的写法：hfa 的中文需要拼音，日文需要罗马字，英文则需要普通单词。
- scheme 与音素表相互独立：yousa 的 `cmn-pinyin` 使用 opencpop-extension，hfa 使用 ds-zh-pinyin-lite。两者 615 个共同音节的映射一致，hfa 的音素表是前者的子集。宿主必须依据声明的音素表判断对齐结果能否直接用于某个歌手，而不能依赖假设。
- configuration 为变体私有，宿主不读取；而调用方未指定时采用的默认值正是宿主需要获知的信息，因此属于 exports（依据 A17 的分工）。
- Align 当时尚无宿主使用，且 A26 已进行不兼容的改名，因此一并定型，不另行递增 Level。

### A28 — 2026-09-29 代码质量审计后的契约修正

本条记录 2026-09-29 联合审计（硬编码、设计缺陷、代码异味）之后契约层面的变化。

**决策**：
- `otter/Support` 不再属于公共面：头文件不安装，读取器不从共享库导出，而是编译进私有静态库 `otter_support`，由库与各插件各自静态链接。安装后的包中没有 `otter/Support`，CI 的消费者工程（`.github/consumer/CMakeLists.txt`）断言这一点。A32 把这个库移出 `include/`，改为 `src/util/otterutil`，A33 再改名为 `analysisutil`。
- 停止语义保持不变并写入文档：执行检测到停止请求后 `start()` 返回 `AnalysisError::Cancelled`，不返回部分结果，`state()` 为 `Canceled`。这与 wolf linguist「停止后返回已完成部分、状态为 `Canceled`」有意不同。
- 执行结束后的状态只由错误码决定：`Cancelled` 对应 `Canceled`，其他错误对应 `Failed`，有结果对应 `Succeeded`，不再参考停止标志；工作线程在回调前也不再将状态改为 `Canceled`。
- `AnalysisError` 新增 `Internal`（执行体抛出异常、解释器在加载期建立的声明对象缺失、产出其他契约的执行体或结果）与 `ModelFailed`（模型输出缺失、元素类型不符、尺寸与输入或其他输出不一致）。这两类错误此前报 `InvalidFormat`，与「声明不合格」混为一谈。取消错误统一由 `otter::cancelledError()` 构造。
- `srt::ITask` 的公开入口 `start()` / `startAsync()` 在类型转换前校验输入的 `type()` / `version()` 等于 `StartInput::Contract.interfaceId()` / `StartInput::Contract.level()`（各 StartInput 与 Result 类引用同一个 Contract 对象），不符时报 `InvalidArgument`，且在认领执行之前拒绝，不改变状态。类型化入口将结果转换回契约类型前同样校验，不符时报 `ContractViolation`，结果为空时报 `Internal`。`Executive::create()` 以 `dynamic_cast` 取代 `static_cast`，失败时报 `ContractViolation`：第三方变体按设计可以提供解释器，其返回的执行体不能被假定属于本契约（A30 修订：结果不符与 `dynamic_cast` 失败原报 `Internal`）。
- hfa 变体要求 `exports` 声明 `silenceLabel`，并要求它既在词表的 `silent_phonemes` 中，也是词表 `vocab` 的一个类。词典展开歌词时以它作为词间分隔符，解码时将它的类作为静音类；该类的下标从词表读取，不再像参考实现那样假定为 0。模型自带的 `config.json`、`vocab.json` 与 `exports` 所列语言的词典在加载期由 `createConfiguration` 读取一次并由 configuration 持有，创建分析器时只打开图；只为 `exports` 声明的语言装载词典，configuration 额外映射的语言不再要求有词典。音频短于模型的一帧时以 `InvalidArgument` 拒绝，不再返回空结果。
- Note：调用方给出的语言必须属于 `exports` 声明的语言（game 先查 `exports.languages`，再查 configuration 的编号；桩变体在未声明任何语言时也拒绝，与头文件中「Must be one the module declares」一致）。`knownNotes` 除原有的三条约束外，还不得结束于片段之后；game 与桩经同一个私有检查函数（`otter::support::checkKnownNotes`），在任何模型运行之前拒绝。
- F0 的清音插值移入私有库（`otter::support::interpolateUnvoiced`），由 rmvpe 与桩共用：锚点为频率大于 0 的浊音帧，频率为 0 Hz 的浊音帧既不作锚点也不被改写，其两侧照常插值（此前会使其后的整段清音保持模型原值）；插值单遍完成，不再对每个清音帧重新扫描整段。
- `otter::PreparedSamples::prepare()` 的返回类型由 `std::vector<float>` 改为 `otter::PreparedSamples`：单声道片段以调用方缓冲区的视图返回，不再整体复制；多声道片段仍平均到自有缓冲区。视图的有效期与 `AudioSegment` 相同。
- linter 与加载器对齐，原则是「加载器拒绝的声明，linter 必须拒绝」：补齐变体事实（rmvpe 16000 Hz / 0.01 s、 game 44100 Hz、三个变体均为单声道）、hfa 的 `configuration.languages` 必填、`config.json` 的 `hop_size` 与 `vocab.json` 的三个必需键、`silenceLabel` 必须声明且是词表的类、game 的 `timestep > 0` 与 `scheduleStart ∈ [0, 1]`、整数上界 2^31−1、包 id 与贡献 id 的文法、版本号各段不超过 int，以及写给本包分析器的 `imports[].options` 非空即拒绝；相对路径中的 `\` 按 `/` 解析。版本文法与加载器一致，为 1 至 4 段，不要求四段（已发布的包使用四段，但加载器不作要求）。自测逐条覆盖上述拒绝规则，并断言键表与 `docs/schemas` 一致；三份 exports JSON Schema 为 `sampleRate`、`channelCount` 与整数旋钮补上与读取器一致的上界。 `make-package.py` 改用 linter 的 JSON 读取器与 `MODEL_KEYS`，分块写入，不再因文件名相同而使伴随文件相互覆盖；同一输入两次打包的结果逐字节一致。
- `AnalysisTask<Input, Result>` 的通用线程与状态逻辑由 `srt::ITask` 统一提供。`otter::AnalysisTaskBase` 只保留载荷身份验证、`AnalysisError::Cancelled` 分类和异常/worker 错误映射；模板负责 typed body、结果转换和 typed callback。`startWorker()` 仍可覆盖以测试建线程失败。三个分析契约与所有提供者共享同一套 claim、callback、等待和 active-only stop 语义。
- 三个 ONNX 提供者重复的会话外壳（stop / wait 转发、取消翻译、驱动获取与会话创建、模型调用与输出检查、 `readFloats`、路径读取、波形张量、插件守卫）移入私有静态库 `otter_onnx_support`（今为 `onnxutil`，见 A33）；插件的 CMake 目标由函数 `otter_add_interpreter_plugin` 声明，桩插件改名为 `otterstub`（A33 起为测试里的静态插件）。桩实现 Align 契约，使 Align 读取器在不含 dsinfer 的构建中也有测试覆盖。
- 以上改动不改变当时三个出厂包的声明（历史）：随 `models-v0.3.0.0` 发布的 `otter-rmvpe`、`otter-game`、`otter-hfa` 照常载入，版本保持 0.2.0.0；A31 起包版本回到 0.1.0.0，现行发布为 `models-v0.1` 下的四个包。

**依据**：
- 读取器是库与插件之间的实现细节，此前却随包安装，并作为导出符号出现在共享库中，宿主因此可能依赖一个从未承诺的接口。读取器是无状态的纯函数，每个模块各持一份不会引入重复的进程级状态，因此可以静态链接并完全隐藏，无须维护一个「内部导出」的符号面。wolf 同样不安装 `wolf/Support`。
- 部分分析结果在形式上与「覆盖较短音频的完整结果」无法区分（一段曲线、一串音符、一组对齐词），宿主无从得知结果覆盖了哪一部分；wolf 返回的是已转换完成的词，可以原样使用。两库的差异因此是有意的，并写入 `AnalysisExecutive::stop()` 的注释。
- hfa 此前在每次创建分析器时重读全部模型文件与词典（cmudict 约 13.4 万行），且为 configuration 中的所有语言装载词典；解码隐含「词表第 0 类是 SP」的假设而加载期不校验，分隔符 `"SP"` 在 G2P 与解码中硬编码，可能与声明的 `silenceLabel` 不一致。
- 若状态由停止标志决定，当执行体因校验失败返回 `InvalidArgument` 而停止请求恰好随后到达时，会出现 `state() == Canceled` 而错误码为 `InvalidArgument` 的情形，与「调用方按错误码分支」的约定矛盾。
- 公开入口中未经检查的 `static_cast`：`start(const TaskStartInput &)` 是 `srt::ITask` 的公开接口，任何契约的载荷都可能到达；`TaskPayload` 的 `type()` / `version()` 正是为此而设。
- linter 与加载器之间的差异使「lint 通过而装载被拒」成为可能，而打包阶段是修正声明的唯一时机。

### A29 — `tifa` 提供者的取舍：`lyrics="scheme"`、模型自带音素表、五图会话、诊断缓办

**决策**：第四个出厂变体、也是第二个对齐变体 `tifa`（TIFA 强制对齐器）按下列取舍落地。算法与实测细节留在 [plans/TifaAlign.md](plans/TifaAlign.md)，这里只登记契约面与取舍。

- **四门语言全部 `lyrics="scheme"`**（`cmn`/`pinyin`、`yue`/`jyutping`、`jpn`/`romaji`、`eng`/`arpabet`， `defaultLanguage` 为 `cmn`）：歌词由调用方按该语言的 scheme 写法给出，变体不做「文字 → 书写单位」的转换。
- **音素表取自模型自带的 `vocabulary.json`，`phonemes` 承诺裸音素**：加载期将每门语言的 `phonemes` 与词表中带该语言前缀的符号集合去前缀后逐一比对，多一个少一个都拒绝载入（A27 的规则）。宿主语言标识到模型语言码的映射（`cmn→zh`、`yue→yue`、`jpn→ja`、`eng→en`）在 `configuration.languages` 中（A11）。
- **静音与非语音如实声明**：`silenceLabel` 为 `"SP"`，但模型词表中没有这个符号——它是变体给「没有词的片段」的拼写，加载期只要求它不与声明的音素重名。`nonSpeechPhonemes` 为空：模型没有非语音检测头，因此声明「不检测」，不拿词表中那几个无前缀标签充数。
- **五张 ONNX 图各开一个会话**（`spectrogram`、`model`、`prepare`、`score`、`select`），按「音频前端 → 候选打分 → 选择 → 取对齐特征」的顺序驱动，其中 `model` 进图两次；图外还有一层由变体自己实现的候选选择与解码。
- **一次执行一个样本、一个 span**，没有 batch，切片仍归宿主（A7）。
- **诊断类输出暂不提供**：发音候选与分数、四个无参考诊断指标算在变体内部但不经契约回传——Level 1 的 `Result` 只有 `words`（A22），没有承载它们的通道。按迁移方案的 D2 缓办。
- **模型词表中查不到的书写单位按未收录处理**：丢掉该候选、丢掉该词，整句没有一个可用词时才拒绝这次执行。与 `hfa`（词不在词典就跳过）相比，它多覆盖一种情形——候选中含词表拼不出来的音素。

**依据**：
- 上游将「书写单位 → 音素」做成随模型发布的数据，而「文字 → 书写单位」是另外三套转换器（拼音、日文分词、英文 LSTM-G2P）。只带词典就够四门语言使用，且与 wolf 的 G2P 分工不重复——宿主手里本来就有 scheme 写法的输出，这正是 A27 让 `lyrics` 可以取 `scheme` 的用意。
- 音素表以模型自带词表为权威，因为宿主要用它判断对齐结果能否直接交给某个歌手（A27 的依据）；承诺裸音素，模型自己的语言前缀不进结果。
- 五张图的分工由上游规定、发布物中没有端到端图，将它们接起来的是变体自己——与 A3「解释器直接驱动 ONNX」一致，也解释了为什么这个变体的会话数与另外三个不同。
- 诊断指标没有承载体（A22），现在增加字段等于凭空猜语义，与第 11 章「`Align` 的置信度」同一条口径。
- 词表查不到时照常喂进图，模型会拿到 padding 编号并**静默给出错误对齐**，比拒绝执行坏得多；而上游默认的「丢掉整条样本」对宿主过于激进，故取「丢词、不丢整句」这一侧。

**代价（已接受）**：`lyrics` 的写法与语言强绑定，调用方必须自己（或经 wolf 的 G2P）将文字转换为 scheme 写法；发音候选与诊断指标在 Level 1 上拿不到。

**来源**：迁移方案的 D2、D3、D9 与其中的差异登记（用户选项确认）。

### A30 — 错误码按条件拆成六个取值

**决策**：`AnalysisError` 从四个取值扩展为六个，每个取值只对应一种条件。`Internal` 收窄为「执行体抛出异常、执行体没有产出结果、解释器在加载期建立的声明对象缺失」；新增 `ContractViolation`，表示产出了其他契约的执行体或结果；`ModelFailed` 收窄为「模型没有产出变体要读的输出（缺失）」；新增 `ModelMismatch`，表示输出存在但元素类型、形状或元素数不符。读取输出时「为空」与「类型不符」不再合判，两种情况各自的码与消息都只说明自己那一种。包自身的文件（`manifest.json`、`config.json`、词汇表、词典、exports 声明）与变体不符时仍用框架的 `srt::Error::InvalidFormat`，宿主传参不合法时仍用 `srt::Error::InvalidArgument`，两者都不属于本类别。「能力缺失」这类条件同样以 `srt::Error::InvalidArgument` 拒绝，如宿主请求了模型没有声明的非语音标签：不合法的是这次请求本身，与「包与变体不符」是两回事，故不取 `FeatureNotSupported`。例外是「不支持」的一方不是包与变体，而是运行它的一份构建、驱动或插件：驱动侧的三项在 `openSession` 中检查（本单元没有注册 ONNX 驱动、注册的服务不是驱动、驱动未能建立会话），插件只服务某个接口/级别/变体则由独立的 `checkServed` 比对，与驱动无关；这些仍用 `srt::Error::FeatureNotSupported`。库自身只支持单声道是执行期限制（`PreparedSamples::prepare`，取 `srt::Error::FeatureNotSupported`），而四个出厂变体在 `createExports` 里已按「包与变体不符」于装载期以 `srt::Error::InvalidFormat` 拒绝声道数不为 1 的声明。桩没有这一层检查，按 2 声道声明的包仍能装载，直到第一次执行才被库拒绝。「能力缺失」的拒绝消息同样是一句话：说清调用方请求了什么、本模型做不到什么，句尾的「做不到」由本模型承担（`which this model cannot <动词>`），不写包、不写环境；`tifa`、`hfa` 与桩拒绝未声明的非语音音素时即 `the caller asked for the non-speech phoneme <标签>, which this model cannot report`。

**依据**：A28 引入的 `Internal` 与 `ModelFailed` 各自混合了多种互不相同的条件，宿主只能把它们一并当作「模型跑不动」处理，而「模型文件不完整」与「模型与变体不匹配」需要不同的处置。spec 2.4 要求契约规定其错误语义，一个取值对应多种条件时，宿主无法按码分支。

**代价（已接受）**：宿主需要识别两个新取值；读取输出的代码多一次分支。`level` 保持 1：错误码位于库级的 `AnalysisError.h`，不在按 level 版本化的 `Api/*/1/` 目录内，既有包因此不需要重新声明。

### A31 — 按消费者把 Analysis 拆成宿主面与提供者面

**决策**：`include/otter/Analysis/` 下只保留宿主面与共用的错误类别（`AnalysisError.h`、 `AnalysisExecutive.h`），解释器实现者需要的三个头移入 `include/otter/Analysis/Provider/`： `AnalysisInterpreter.h`、`AnalysisTask.h`、`AnalysisInput.h`。两个面都安装且都是公共头，提供者面不并入不安装的私有库（今为 `analysisutil`，见 A32、A33），因为树内插件与第三方变体都要读它们。`AnalysisInterpreter.h` 不再传递包含 `AnalysisExecutive.h`，需要该类型的实现者显式包含它。

**依据**：一个目录同时服务两类受众时，宿主读到的是为提供者写的规则。宿主真正需要的只有执行体与错误码，把它与提供者要实现的解释器、任务与输入校验分开后，宿主面的符号从三十余个降到约九个。宿主面的路径因此不变，已接入 otter 的宿主不需要改动任何 include。

**代价（已接受）**：全仓 include 路径一次性改动。三份契约头仍要包含提供者面的 `AnalysisTask.h`，因为它们的 `XxxExecutive` 持有该模板的成员，所以提供者面也会跟着契约头进入宿主，目录名说的是这个面**为谁准备**，不是包含方向。

提供者面不含 `AnalysisInterpreter` 上回答自身标识的三个访问器（`interfaceName()`、`level()`、`variant()`）：变体名与级别在装配处已经确定，实现者不必再提供一遍。

**来源**：契约面清点（C1 的 S1、S2）与用户选项确认。

### A32 — 私有工具库移到 src/util/otterutil

**决策**：库与插件共用的读取器与检查（`ManifestValues`、`F0Curve`、`KnownNotes`）移出 `include/otter/Support`，放到 `src/util/otterutil`，结构与 dsinfer 的 `util/inferutil` 相同：头文件在 `include/otterutil/` 下，引用写作 `<otterutil/X.h>`，私有静态库由 `otter_support` 改名为 `otterutil`。C++ 命名空间不变。`include/` 下只剩公开头文件，安装直接调用 `otter_install()`，不再需要 `NO_INCLUDE` 与排除 `Support` 的自定义安装规则。消费者工程与安装面检查改为断言安装树中没有 `otterutil` 的头文件与静态库。

**依据**：私有头文件放在公开的 include 树里，安装时必须逐条排除，`Support` 这个名字在 synthrt 与 dsinfer 中又指公开头文件，含义相反。dsinfer 把同类的私有库放在 `util/` 下，公开 include 树里只有公开头文件，安装规则不需要例外。

**代价（已接受）**：库、插件与测试的 include 路径一次性改动，两个测试移到 `src/tests/auto/util/otterutil/`。

**来源**：用户确认，与 dsinfer 的 util 结构对齐。

### A33 — 私有工具库按功能命名，测试桩改为测试里的静态插件

**决策**：
- `src/util` 下的私有静态库按功能命名。`otterutil` 改名为 `analysisutil`。四个 ONNX 提供者共用的 `otter_onnx_support` 从 `src/plugins/inferenceinterpreters/onnx` 移到 `src/util/onnxutil`，目标名 `onnxutil`。两个 ggml 提供者共用的 `otter_cli_support` 从 `src/plugins/inferenceinterpreters/external` 移到 `src/util/ggmlutil`，目标名 `ggmlutil`。三者的结构相同，引用写作 `<analysisutil/X.h>`、`<onnxutil/OnnxSupport.h>` 与 `<ggmlutil/X.h>`。
- `otter_cli_support` 自带的子进程类删除，`game-ggml` 与 `tifa-ggml` 改用 stdcorelib 的 `stdc::Popen`。`ggmlutil` 只保留交给引擎的文件（`EngineFiles.h`：float32 WAV 与 scratch 目录）和管道的平台差异（`EnginePipe.h`：Windows 上不为引擎打开控制台窗口，POSIX 上写管道不触发 SIGPIPE）。
- 取消语义不变。`stop()` 可以从另一个线程杀掉正在读写的引擎，析构时终止仍在运行的引擎。`stdc::Popen` 的成员函数没有同步，所以每个提供者用一把锁串行化对它的调用。管道的读写不持锁，`stop()` 持锁 kill 之后，阻塞中的读写随即失败。只有执行线程创建、替换和释放引擎，释放时在锁内把指针移出，由析构函数 kill 并等待子进程。`tifa-ggml` 等待工具结束时按 50 ms 分片，每片在锁内调用 `wait()`，`stop()` 在两片之间拿到锁。
- 测试桩 `otterstub` 不再是插件目录。代码移到 `src/tests/auto/plugins/stub`，以 `STDC_EXPORT_STATIC_PLUGIN` 注册为 inference 类别的静态插件，清单写在代码里，内容与原来的 `plugin.json` 相同。它编成 OBJECT 库 `otterstub`，由 `test_AnalysisLoad` 与 `test_AnalysisRuntime` 链接，静态库会丢掉没有被引用的注册对象，所以不用静态库。两个测试不再设置插件路径，`OTTER_TEST_PLUGIN_DIR` 删除。`test_AnalysisLoad_ServesTheStubAsAStaticPlugin` 断言插件路径为空时测试包仍能装载。测试包里的 `"variant": "stub"` 不变。
- 安装面检查与消费者工程断言三个私有库都不在安装树中。

**依据**：`otterutil` 只说明它属于 otter，不说明它做什么，而三个私有库服务的是三组不同的代码：库与全部插件、ONNX 提供者、ggml 提供者。按功能命名后，名字本身就说明了谁链接它。两个共用库原先与插件并列放在插件目录下，看上去也像插件。子进程管理 stdcorelib 已经提供，自带一份就要自己维护三个平台的实现。桩只服务测试，作为插件目录时它要构建进插件树，要用 `NO_INSTALL` 排除出部署，测试还要知道插件目录在哪里。改为静态插件后这些都不需要了，插件树里只剩可部署的提供者，`otter_add_interpreter_plugin` 也不再需要 `NO_INSTALL`。

**代价（已接受）**：库、插件与测试的 include 路径与目标名再改一次。`stdc::Popen` 只在 `communicate()` 内防护 SIGPIPE，提供者直接写管道，所以 `ggmlutil` 自己补上这层防护。`stdc::Popen` 不能从另一个线程直接调用，所以每个提供者要自己加锁，`tifa-ggml` 的 `stop()` 最多等一个分片。ctest 中不再有测试从插件目录装载 otter 的插件，插件树的发现与装载只由消费者工程覆盖到目录本身。

**来源**：用户确认。


### A34 — 契约身份与变体配置分离

2026-11-17：公开契约使用 ContribContract 对象统一 interface 与 level，类型名去掉命名空间已表达的前缀。Exports、ImportOptions、RuntimeOptions 不接收 variant，配置仍保存完整 ContribContractTriplet。契约解析函数不再接收 variant。输入与结果通过静态 Contract 引用向 AnalysisTask 提供身份。插件内部 Triplet 保存具体实现身份，库与插件及宿主须配套重建。实现与迁移说明见 [ContractApiMigration.md](codex/ContractApiMigration.md)。

## 9.5 联合审计（实施后）

实施完成后，对 otter、synthrt 与 wolf 三层进行了联合审计，审计维度为稳定性、向后兼容、规范符合度与并行安全。synthrt 与 wolf 无新发现。otter 发现七项问题，均已修复，且每项都补充了能复现原缺陷的用例。

| 编号 | 类别 | 问题 | 处置 |
| :-- | :-- | :-- | :-- |
| **X1** | 正确性 | `channelCount` 读入成员后未被使用，两个提供者无条件降混，声明为 2 声道的模型会被静默送入单声道输入 | 校验下沉到 `otter::PreparedSamples::prepare()`，模型声明的声道数无法满足时直接拒绝 |
| **X2** | 正确性 | 未校验 `samples.size()` 是否为整帧数，尾部不完整的帧被整除丢弃，表现为后续的时间漂移而非错误 | 同上，拒绝而非丢弃 |
| **X3** | 契约 | `knownNotes` 的时间基准不一致：头文件规定与音频同一时间轴（绝对时间），实现却按相对于片段起点处理，而输出为绝对时间。宿主分析第一段时结果无差异，之后每一段均出错且无任何提示 | 统一为绝对时间；提供者减去 `startTime` 并校验落在片段内。用例改为 `startTime = 30` |
| **X4** | 规范符合度 | `createImportOptions` 无条件报错，使 spec 2.4 允许的情形变为加载失败；规范明确允许 import 一个不提供 Factory 的类别的贡献，加载器也接受空工厂 | 返回空 options 与惰性 binding；只有显式写出 options 时才拒绝（Level 1 契约不读取 import options） |
| **X5** | 健壮性 | `AnalysisRunner::spawn` 创建线程抛出异常时 `running` 永久为真，分析器此后拒绝一切执行 | 捕获异常并释放认领；任务面中由 `startAsync()` 处理，`startWorker()` 失败时返回 `NoWorker` |
| **X6** | 契约 | Exports 声明了旋钮的取值域但无任何校验，`steps = -1` 会生成空张量并送入模型 | `Knob::choose()` 按声明的取值域校验，越界即拒绝，不做钳位 |
| **X7** | 测试有效性 | 桩提供者不做任何校验，与真实提供者行为不同，针对桩编写的契约用例因此不能证明契约成立 | 桩改为调用同一套库函数；X1、X2、X6 正是在桩上的用例失败后暴露的 |

X1、X2、X6 的共同原因是两个提供者各自实现同一套校验，实现必然产生分歧。与 `AnalysisTask` 相同，校验也下沉到库中（`otter/Analysis/Provider/AnalysisInput.h`），各实现因此保持一致。

## 10. 实施里程碑

| 里程碑 | 内容 | 状态 |
| :-- | :-- | :-- |
| **M1** | 仓库骨架：CMake、vcpkg 清单与 port | **完成**。`find_package(synthrt-otter)` 可从安装树使用；CI 消费者工程校验 `synthrt::otter` 目标与公共头文件，断言安装树不含 `analysisutil`、`onnxutil` 与 `ggmlutil`，并运行程序检查从导出的插件目标读出的插件目录 |
| **M2** | 契约头与加载路径（最初为 `analysis` 类别，A26 后改为 inference 模块） | **完成**。`test_AnalysisContrib` / `test_AnalysisLoad` / `test_AnalysisRuntime` |
| **M3** | `rmvpe` 提供者 | **完成**。`test_RmvpeInterpreter`，运行于 ONNX 图 fixture |
| **M4** | `game` 提供者，补回 `dur2bd` | **完成**。`test_GameInterpreter`，含对齐路径 |
| **M5** | 打包 lint 与模型 fixture | **完成**。`scripts/check-declarations.py`、`scripts/make-model-fixtures.py`、`scripts/make-package.py` |
| **M6** | lite 接入 | **完成**。L1 至 L6 六个阶段全部实现，见 [LiteIntegration.md](LiteIntegration.md) |
| **M7** | `Align` 契约与 `hfa` 提供者 | **完成**。契约头、提供者、fixture 用例与打包脚本均已实现；0.1.0.0 版 hfa 包曾随 [models-v0.2.0.0](https://github.com/diffscope/otter/releases/tag/models-v0.2.0.0) 发布，其后的 0.2.0.0 版曾由 `models-v0.3.0.0` 提供（历史，两个标签均已删除）；A31 起版本回到 0.1.0.0，现行发布为 `models-v0.1`；实测与决策见 [plans/HfaAlign.md](plans/HfaAlign.md) |
| **M8** | `tifa` 提供者 | **完成**（P5 诊断指标与 P6 完整 G2P 按 D2/D3 缓办，属分期决策而非漏做）。声明、插件（五张图各一会话、加载期互检）、候选打分路径、帧解码与时间轴均已落地，`test_TifaInterpreter` 随夹具一起通过。**实测门禁：宿主调用形态（逐 take）下与参考逐边界一致**；batch-8 口径的差异归因于参考自身对批组成敏感（不是移植缺陷）。两种口径的读数、口径裁决与迁移记录见 [plans/TifaAlign.md](plans/TifaAlign.md) §11.10–§11.11 |

### 测试集

当前 ctest 共注册 30 个测试，其中两个真实 GGML 引擎用例在缺少对应引擎时跳过；完整名称与条件以各 `tests/auto/CMakeLists.txt` 为准。下表列出主要入口：

| 测试 | 内容 | 条件 |
| :-- | :-- | :-- |
| `test_ContribInterpreterPlugin_Otter` | 链接 otter 不注册任何类别 | 始终构建 |
| `test_AnalysisExecutive_Load` | 经静态插件形式的测试桩（`otterstub`）的包加载，不设插件路径、`DataOnly` 模式、声明读取与拒绝规则、其他模块对分析器的引用 | 始终构建 |
| `test_AnalysisExecutive` | 经静态插件形式的测试桩的分析器创建、执行、旋钮、输入校验、取消、callback 再启动/销毁和共享 Task 语义 | 始终构建 |
| `test_ManifestValues` | 私有声明读取器 | 始终构建，链接 `analysisutil` |
| `test_F0Curve` | 清音插值 | 始终构建，链接 `analysisutil` |
| `test_RmvpeInterpreter` / `test_GameInterpreter` / `test_HfaInterpreter` / `test_TifaInterpreter` | 四个提供者在生成的 ONNX fixture 上的完整链路：链接各插件的 `<target>_objects`，以插件的 `plugin.json` 把被测解释器注册为静态插件，不从插件目录加载插件 | 仅在含 dsinfer 的构建中存在；没有带 onnx 与 numpy 的 Python 时注册为禁用；运行期缺少 fixture、驱动或运行时时以退出码 77 报告跳过 |
| `test_OnnxSupport` | 四个提供者共享的 ONNX 读取器与执行器封装：输出缺失与类型不符的区分，空结果与缺少输出的拒绝 | 仅在含 dsinfer 的构建中存在（要求 `onnxutil` 目标） |
| `test_CheckDeclarations` | `scripts/test_*.py`（当前两份：声明 linter 与打包脚本的自测；测试包生成器没有自测） | 找到 Python 解释器时注册 |
| `test_InstalledSurface` | 安装树形态（`check-installed-surface.cmake` 装入 scratch 前缀），含不得安装的私有头文件 | `OTTER_INSTALL` 打开时注册 |
| `test_Decode` | tifa 的逐帧解码，期望值取自参考实现，链接 `ottertifa_objects`，不需要模型 | 仅在含 dsinfer 的构建中存在 |
| `test_G2p` | hfa 的发音词典：分隔符的位置、词典缺词、条目首尾的分隔符、CRLF，链接 `otterhfa_objects`，不需要模型 | 仅在含 dsinfer 的构建中存在 |

### 验证状态

**已验证**：包加载（含 `DataOnly`）、解释器发现、链接 otter 不引入类别、经 `Executive::create` 创建执行体、同步与异步执行、取消、输入校验、旋钮传递、四个提供者在 ONNX 图上的完整链路、对齐路径确实运行 `dur2bd`、 `find_package(synthrt-otter)` 从安装树消费、不含 dsinfer 的最小构建正常降级。

**已用真实权重验证**：
- **GAME**：三个合成音 A3 / C4 / E4 转写为 MIDI 57 / 60 / 64，边界位于整秒处，时长与占空比吻合。该验证同时表明，fixture 原先依照提供者的假设声明张量形状，因此测试始终通过而任何真实导出的模型都无法运行； A10 所列的形状修正即由此得出。fixture 现按真实导出编写。
- **HFA**：v0.0.7 权重经打包后的包，在 `BaiMaRuLuHua_0.wav`（中文）上与参考实现 `dstools-cli hfa` 逐音素比对区间：27 段，标签序列一致，最大边界差 7.5e-7 s（帧长 10 ms）。命令与产物见 [plans/HfaAlign.md](plans/HfaAlign.md) 的真实权重验收一节。
- **TIFA**：四门语言均通过等价门禁——11 条真实素材 + 31 条合成素材共 42 条，对参考实现音素与词标签序列逐个相等、**逐边界零差异**（判据用宿主的调用形态：一次一个 span）。口径裁决与读数见 [plans/TifaAlign.md](plans/TifaAlign.md) §11.10–§11.11。

**未验证**：
- RMVPE 的数值。权重随 rmvpe 包发布（现行版为随 `models-v0.1` 发布的 0.1.0.0 版；此前的 0.2.0.0 版在 `models-v0.3.0.0` 中，该标签已删除），但未与参考实现进行数值比对；相关用例在缺少 fixture 时报告跳过，而不是报告通过。
- HFA 的英语与日语词典。词典是数据而非代码，但缺少对应素材，无法验证；`merged_phoneme_groups` 路径按 C++ 参考实现的行为对齐，与 Python 版本不一致（见第 11 章）。
- TIFA 的英语与粤语真实素材。真实录音只覆盖 `cmn`（5 条）与 `jpn`（6 条），`eng`/`yue` 的等价性由合成素材覆盖，尚无真实录音进场。四门语言共 42 条的读数见 [plans/TifaAlign.md](plans/TifaAlign.md) §11.11。

## 11. 未决事项

- **`Transcribe` 的契约面。** 只登记 `interface` 名称与所属类别，不定义类型。（`Align` 已由 A19 定义。）
- **`Align` 的等价性与跨语言覆盖。** `hfa` 与参考实现（dataset-tools 的 `dstools-cli hfa`）的数值等价性只在一套中文素材上比对过；英语与日语词典缺少素材，未经验证。参考实现的 Python 版与 C++ 版在 `merged_phoneme_groups` 上存在差异（C++ 版未使用该字段），本轮按 C++ 版的行为对齐。
- **`Align` 的置信度。** Level 1 不含逐词置信度。参考实现在解码时计算帧置信度与段总置信度，这些数据能否支撑「某个字对齐质量不佳」这一层信息，需要在宿主实际使用后确定；在此之前增加字段，其语义缺乏依据。
- **`tifa` 的诊断输出与完整 G2P（A29）。** 发音候选与分数、四个无参考诊断指标算在变体内部、不经契约回传；「文字 → 书写单位」的转换也按 `lyrics="scheme"` 留在调用方。是否为前者扩展 Align Level 1、是否将后者补成完整 G2P（会引入分词与 LSTM-G2P 这类依赖），都等宿主实际使用后再定。
- **`AnalysisSession` 的必要性。** wolf 以 `LinguistSession` 承载目录、就绪状态与执行体池。otter 的抽参器数量少且一次只运行一个，暂不实现。lite 接入后，获取分析器、读取 schema、准备音频、切片与逐片执行这些步骤确实重复；但三个任务的后半部分（结果形式与时间轴换算）并不共用，下沉之前需要先明确下沉的范围。
- **跨段边界连续性。** segmenter 的 `prev_boundaries` 是**采样循环自身的状态**：每一步将上一步的输出送回，与上一段音频无关（已由真实模型确认）。因此该模型没有现成的跨片连续性入口。对齐同理：`hfa` 每段独立解码，跨段的词边界连续性由宿主将切点置于词与词之间来保证。
