# otter 接入 ds-editor-lite（M6）

本文记录 otter 在 lite 侧的接入方案，只涉及 lite 一侧的改动。otter 自身的分层与契约见
[otter-design.md](otter-design.md)，两者冲突时以 otter-design.md 为准。

**范围**：本文覆盖 F0 与 Note 两份契约（`rmvpe`、`game` 两个包）。第三份契约 `Align`（`hfa` 包）
**不在本轮 lite 接入范围内**，因为它需要新的任务与 UI，而非迁移已有的抽参路径。otter 侧的契约、
解释器与包均已就绪，见 [plans/hfa-align.md](plans/hfa-align.md)。

## 前置条件

M6 要求 lite 先迁移到 **synthrt main + wolf**。这是硬约束：otter 的分析器是 synthrt main 线内置
`inference` 类别下的模块（A26；此前为 otter 自行注册的类别），而 lite 接入前链接的是 synthrt 的
refactor 线，两条线的 `SynthUnit` 不是同一类型。lite 完成该迁移之前，本文的各项改动均无法落地。

因此本文的改动并入 lite 的迁移分支，不单独开分支。

## 锚点

| 树 | 观察点 |
| :-- | :-- |
| lite | `main` 分支（迁移前的抽参实现） |
| lite 历史（切片器的来源） | 删除 `audio-util` / `game-infer` / `rmvpe-infer` 之前的 lite 历史 |
| otter | `analysis-level-1` |
| talcs | vcpkg `talcs_x64-linux`，`TalcsFormat` / `TalcsCore` |

---

## 1. lite 现有的抽参实现

```
ExtractPitchTask::runTask()
  ├─ options->general()->rmvpePath           用户在设置中选择的一个裸 .onnx 文件
  ├─ runtime.services().get<PluginFactory>() refactor 线的插件工厂
  ├─ plugins->plugin<PitchExtractorPlugin>("rmvpe")
  ├─ ExtractorUtils::decodeAudio()           srt::audio::AudioPipeline（ffmpeg 解码 + 重采样）
  ├─ extractor->extract(buffer, sampleRate)  插件内部再次重采样并做 RMS 切片
  └─ processOutput()                         freqToMidi，按硬编码的 10ms 间隔重采样到 5 tick 网格
```

`ExtractMidiTask` 结构相同，另有一步 `options.tempo = timeline.tempoAt(0)`。

该路径有三个已知缺陷，迁移时一并修复：

- **变速曲的音符位置系统性偏移。** `tempoAt(0)` 只取第一个速度标记，插件用它把秒换算成 480 PPQ
  的 tick。曲子中途变速时，变速点之后的所有音符位置都错误。
- **`intervalMs = 10.0` 硬编码在 lite 中。** 换用帧间隔不同的音高算法时，lite 无从获知新的间隔。
- **对齐能力不可用。** GAME 的权重支持以已知音符为条件，但 refactor 线的封装丢弃了对应的 session。

## 2. 目标结构与职责划分

| 事项 | 迁移前 | 迁移后 |
| :-- | :-- | :-- |
| 解码 | `srt::audio::AudioPipeline`（ffmpeg） | talcs `AudioFormatIO` |
| 重采样 | 插件内部 | talcs `AudioFormatInputSource::open(bufferSize, rate)`，rate 取自模块声明 |
| 静音切片 | 插件内部 | lite，一套统一参数 |
| 帧间隔 | lite 硬编码 10ms | 读取 `F0Schema::interval` |
| 秒 → tick | 插件内部，单一 tempo | lite 的 `Timeline`，逐点换算 |
| 模型选择 | 在设置中选择 .onnx 文件路径 | 选择一个已安装的分析器贡献 |
| 参数 | 无 UI，全部使用模型默认值 | 不变，仍全部使用模型默认值（见 §6） |

### 音频格式覆盖面

迁移后解码改由 talcs 完成，而 lite 只注册了 `StandardFormatEntry`（libsndfile）与 `WavpackFormatEntry`，
**没有 ffmpeg entry**，表面上支持的格式比 ffmpeg 少。

实际覆盖面不受影响：`ExtractTask::Input::audioPath` 指向**工程中已导入的音频剪辑**，该文件导入时必须
先经过 `FormatManager`。因此抽参所能接触的文件，talcs 均能读取。原有的 ffmpeg 解码是这条路径上的冗余
能力，没有可达的输入需要它。

若以后需要对工程外的任意文件直接抽参，应为 `FormatManager` 增加 entry，与 otter 无关。

## 3. 文件落点

复核时收窄了改动面。原方案新建 `src/libs/AudioAnalysis` 库与 `src/app/Modules/Analysis` 模块目录，
两者均已取消：切片器只供抽参使用，且 lite 的测试约定是直接重新编译 app 源文件，不需要库目标；
`SynthrtEngine` 已是 `SynthUnit` 的门面，列举分析器只需为它增加两个方法，不必另开模块。原方案还按
`Schema` 自动生成旋钮 UI，这是独立需求，不与迁移绑定。迁移的最小形态是继续不传旋钮、全部使用模块默认值，
与迁移前的行为一致。

**新增**（全部位于 `src/app/Modules/Extractors/` 下，不新建库或模块目录）

```
AudioSlicer.{h,cpp}      RMS 静音切片 + 超长片的强制等分。算法取自删除 audio-util 之前的
                         src/libs/audio-util，参数改为以时间表示
AnalysisAudio.{h,cpp}    talcs 解码 + 重采样到分析器声明的采样率，产出单声道
```

**改写**

```
ExtractPitchTask.{h,cpp}
ExtractMidiTask.{h,cpp}
src/libs/SynthrtEngine/SynthrtEngine.{h,cpp}        inference 类别加入 otter 插件目录；新增两个查询方法
src/app/UI/Dialogs/Options/Pages/GeneralPage.cpp    两个 FileSelector → 两个 QComboBox
src/app/Model/AppOptions/Options/GeneralOption.h    rmvpePath 改为存储贡献引用（字段类型不变）
src/app/Automation/ExtractionAutomationAdapter.cpp  modelId 按目录解析，不再按路径解析
```

**删除**

```
ExtractorUtils.{h,cpp}   只做 ffmpeg 解码，由 AnalysisAudio 取代
```

`ExtractorUtils` 在两个 Task 改写时才删除，此前与新文件并存。删除它需要同时修改两个 Task，而两个 Task
依赖 refactor 线，提前删除会使这一步无法单独验证。

## 4. 音频准备

`Modules/Extractors/AnalysisAudio.h` 是一个薄适配器，把 talcs 的读取结果转换为 otter 所需的 PCM 段：

```cpp
namespace Extractors {

    struct PreparedAudio {
        std::vector<float> samples;   // 单声道
        int sampleRate = 0;
        double startMs = 0;           // 第一个样本在工程时间轴上的位置
    };

    std::optional<PreparedAudio> prepareAudio(talcs::AbstractAudioFormatIO *io, double startMs,
                                              double endMs, int sampleRate,
                                              const std::function<bool()> &cancelled,
                                              QString &error);

}
```

实现方式为 `AudioFormatInputSource(io)` + `open(chunk, sampleRate)` + `setNextReadPosition` + 循环读取。
**重采样发生在 `open()` 中**，这一点由 `TestAudioSlicer` 验证：44.1 kHz 的文件按 16 kHz 请求，读回
恰好 32000 个样本（2 秒）。

适配器产出**单声道**而非交错多声道。talcs 的缓冲是分平面的，所有已有分析器都要求单声道，otter 也会
自行对声道取平均；在读取时取平均既省去一次交错，又减少一半数据搬运。契约对两种形式均接受。

`startMs` 报告的是**实际读到的起始帧**，而非请求值。两者在请求被裁剪时不同，而分析器以该值锚定结果，
两者之差会原样成为结果中的位移。

**校验由 otter 负责。** 采样率不匹配、样本数不是整帧、超过 `maxSegmentDuration` 时，执行体均会报错并
给出原因。lite 不重复这些校验，以免两处规则分叉。

## 5. 切片

切片算法取自删除 `audio-util` 之前的 lite 历史中的 `src/libs/audio-util/src/Slicer.cpp`，现位于
`Modules/Extractors/AudioSlicer.h`，命名空间 `Extractors`。边界处理采用 synthrt refactor 线加固后的写法
（空区间的 `argmin` 返回 0 而非断言，参数钳位为非负），不采用更早的带 `assert` 的版本。

参数统一为一套，不按算法区分，因为换算成时间后两套参数几乎相同：

| 参数 | rmvpe（16k） | game（44.1k） | 统一取值 |
| :-- | :-- | :-- | :-- |
| threshold | 0.02 | 0.02 | 0.02 |
| hopSize | 160 = 10ms | 441 = 10ms | 10ms |
| winSize | 640 = 40ms | 1764 = 40ms | 40ms |
| minLength | 500 帧 = 5s | 200 帧 = 2s | **2s** |
| minInterval | 30 帧 = 300ms | 30 帧 = 300ms | 300ms |
| maxSilKept | 50 帧 = 500ms | 50 帧 = 500ms | 500ms |

`minLength` 取两者中较小的 2 秒：切得更细对 rmvpe 无害，而 GAME 有 60 秒上限。

`SlicingProfile` 以时间存储这些参数，按目标采样率实时换算为帧数，因此同一套配置适用于两种采样率。

**切片器必须保证每片不超过 `Schema::maxSegmentDuration`。** 对持续有声的长音频，RMS 切片可能无法切出
足够短的片，因此静音切片之后还有一道强制再分。这一步不可省略，因为 GAME 对超长片直接报错，而不是降级处理。

再分按**等份**进行，而非「切满上限 + 余数」：25 秒按 10 秒上限分为 3 份，每份 8.33 秒，而非
10 + 10 + 5。末尾的短碎片作为输入劣于三份均等的片。片的端点由两端反算而非累加得出，因此相邻片之间
既无间隙也不重叠。

切片不做重叠。重叠要求宿主了解模型如何合并交叠区域，这属于模型的职责。跨片连续性是 otter 的
未决项（见 §11）。

## 6. 模型发现与设置界面

### 包的安装位置

分析器模型包与声库包**共用同一个 `SynthUnit` 与同一组搜索路径**（otter A13）。分析器是 inference
模块，因此 `SynthrtEngine` 初始化时只需将 otter 的插件目录加入 inference 类别的插件搜索路径。
`setPluginPaths` 替换而非追加路径，三个库的目录必须在同一次调用中全部给出：

```cpp
unit.setPluginPaths(srt::InferenceCategory::NAME,
                    {pluginRoot / "plugins/dsinfer/inferenceinterpreters",
                     pluginRoot / "plugins/wolf/inferenceinterpreters",
                     pluginRoot / "plugins/otter/inferenceinterpreters"});
// 包路径与声库共用，已有
```

包加载完成后，`SynthrtEngine` 通过两个方法列举分析器，不另开模块目录：

```cpp
struct AnalyzerEntry {
    std::string packageId;          // "otter/game"
    stdc::VersionNumber packageVersion;
    std::string contributionId;     // "note"
    std::string interfaceName;      // org.openvpi.otter.inference.F0 / .Note / .Align
    std::string variant;
    srt::DisplayText name;          // 声明中的 name，多语言

    std::string reference() const;  // "otter/game:inference/note"，用于持久化
};
```

设置中存储的是 `reference`，而非文件路径。文件路径随安装位置变化，贡献引用不变。

### 安装根错误时的表现（实测，供安装方对照）

包放错目录时宿主**不报错**，只是分析器不出现在列表中。以下两种状态必须区分：

| 现象 | 含义 | 判据（lite 侧） |
| :-- | :-- | :-- |
| `module_state = "unavailable"` | 没有任何包实现该契约 | `SynthrtEngine::analyzers(interface)` 为空 |
| `module_state = "ready"` 但 `available = false`，理由 "…model is not configured" | 契约已有实现者，但**用户尚未在设置中选择** | `settings.general.pitchAnalyzer` / `noteAnalyzer` 为空 |
| 两项均满足 | 可运行 | `available = true` |

常见的两处错误：

1. **`wolf/packages` 不是分析器包的安装根**，而是 wolf 语言包的**依赖查找**根：lite 将它作为 `packagePaths`
   传给 Bootstrap，用于查找声库所声明依赖的语言包。**扫描根**是用户设置的包搜索路径（`SynthrtEngine`
   的 `voicebankPaths` 实参，在 `InferEngine.cpp` 中绑定），`analyzers()` 读取的正是本次扫描的结果
   （"the analysers the last scan found"，见 `SynthrtEngine.h`）。安装根错误时的现象即上表第一行，而且
   **日志中不留任何记录**：包不在扫描列表中，不会被打开，因此没有可报告的失败。
2. **`packages.list` 列出的是声库目录中的包**，分析器包不出现在该清单中。确认分析器是否可用应查询
   `extract.get_capabilities`，而非包清单。

### 声明不合格时的运行期拒载（实测）

包已被扫描到、但声明与 `configuration` 自相矛盾时，解释器在装载期拒绝该包：game 的
`GameInterpreter::createExports` 依次核对采样率、声道数，以及声明的语言是否都有对应编号。打包期的 lint
（`scripts/check-declarations.py`）遵循「加载器拒绝的声明，lint 必须拒绝」（A28），检查格式字段的类型与
取值范围、变体事实（`check_variant_facts`），以及 hfa 这类可将声明与模型自带元数据比对的一层
（`check_align_declaration` 以 `config.json` 的 `mel_spec_config.sample_rate` 核对采样率）。**能在打包期
拒绝的错误必须在打包期拒绝。** 以下拒载发生在 lint 补齐之前，原文为：

```
SynthrtEngine: could not open D:\...\otter-game@0.1.0.0 : failed to interpret module exports:
    the exports declare supportsKnownNotes but the configuration names no durationToBoundary model
```

- 宿主侧的可观测状态：该契约 `module_state = "unavailable"`、`available = false`（reason `… module is
  unavailable`），**另一契约不受影响**（同一次实测中 pitch 仍为 `ready`/`true`）；
- 强制发出命令时返回明确的拒绝：`The note analyzer is not installed: otter/game:inference/note`
  （`file_not_found`，字段 `note_analyzer`）；
- 这与上一小节的「日志中不留任何记录」不矛盾：**未被扫描到**的包不留记录，**已扫描但声明有误**的包
  留有记录，并指明包与原因。

2026-09 的实测链路（lite 隔离副本 + 本仓库 `packages/{rmvpe,game}`，模型按声明路径放置）：
`module_state` 两路均为 `ready`；选中 `otter/rmvpe:inference/f0` 与 `otter/game:inference/note` 后，
`available` 两路均为 `true`；`extract.pitch.start` 与 `extract.midi.start` 均被接受，ONNX 驱动按
**包内声明路径**打开了六个模型（`otter-rmvpe@0.1.0.0/rmvpe.onnx` 与 `otter-game@0.1.0.0/{encoder,
segmenter,estimator,bd2dur,dur2bd}.onnx`），即「声明 → 模型」一段已打通。同一轮中任务随后在下游失败：
`ExtractTask` 报告 "Failed to open the audio file"（`AnalysisAudio.cpp`）。该问题与本仓库无关，属于 lite
侧音频来源路径的问题，另行跟踪。

### 语言标识采用宿主的词汇表

宿主将语言 id **原样**传给分析器（lite：`ExtractionAutomationAdapter.cpp`），不做转换。因此分析器必须
按宿主 id 识别语言，声明的两侧都使用宿主词汇：

| 位置 | 内容 | lite 的取值 |
| :-- | :-- | :-- |
| `exports.languages` | 宿主 id 列表（分析器接受的语言） | ISO 639-3：`cmn` `eng` `jpn` `yue`（`src/app/Global/AppGlobal.h`） |
| `configuration.languages` | 键为宿主 id，值为**模型自己的**编号 | 例：`{"eng":1,"jpn":2,"yue":3,"cmn":4}` |
| `exports.defaultLanguage` | 宿主 id，调用方未指定语言时使用（A27，原位于 configuration） | `cmn` |

不同模型对「编号 1 表示哪种语言」的规定不必一致，因此编号表属于 `configuration`；`exports` 只列出分析器
能够处理的宿主语言。反例（本仓库 `0.1.0.0` 的第一版声明，已修正）：声明使用模型词汇 `zh/en/ja/yue`，
宿主传入 `cmn` 时查表未命中，任务因模型没有 `cmn` 的编号而失败（当前实现报告
`this model has no number for the language cmn`），宿主原样上报。F0 契约没有语言项（RMVPE 无此字段），不受此规则影响。

### 声明的音频格式是承诺，分析器必须自行校验

`exports` 中的 `sampleRate` / `channelCount`（F0 另有 `interval`）是**宿主据以准备音频的承诺**。契约
有效不等于模型能够履行该承诺：**模型无法履行时必须在 `createExports` 中拒绝**，否则错误体现在数据中，
而不是错误码中。

该错误的后果已经实测（lite 2026-09，同一份代码、同一段音频，只修改已安装包声明中的采样率）：

| GAME 声明 | 结果 |
| :-- | :-- |
| `44100`（模型真实值） | 594 个音符，中位音高键 64 |
| `22050`（错误声明） | **569 个音符，中位音高键 76（整一个八度）**，首音位置与跨度几乎不变，**日志无任何异常** |

即错误的声明导致结果静默升高一个八度，在编辑器中表现为一次正常的抽参。声明的采样率低于实际值时，模型
相当于以两倍速度读取音频，频率加倍，恰好 +12 半音。数值与理论一致，说明该现象不是噪声。

拒绝的写法（两个解释器形式相同，`InvalidFormat` / `FeatureNotSupported`）：

```
the rmvpe variant runs at 16000 Hz; the exports declare 44100      // rmvpe: sampleRate + interval
the game variant runs at 44100 Hz; the exports declare 22050       // game: sampleRate + channelCount
```

### 设置界面

`GeneralPage` 的两个 `FileSelector` 替换为两个下拉框，分别按 `interfaceName` 过滤出音高分析器与音符
分析器。**本轮不做旋钮 UI。**

设计理由是范围控制：迁移前 lite 没有任何抽参旋钮，全部使用模型默认值；迁移后 `StartInput` 的旋钮一律
不填，行为与迁移前完全一致。按 `Schema` 自动生成设置界面是一项独立的需求，不属于迁移。将其并入本次改动
会增加一整套 UI 代码，而该代码的正确性与迁移是否成功无关。契约侧已提供所需信息（`Knob` 带取值域与
默认值），可在以后单独实现。

### 旧设置的处理

原计划保留 `rmvpePath` 一个版本，启动时提示一次后清除。落地时改为：`rmvpePath` / `gameDir` 直接替换为
`pitchAnalyzer` / `noteAnalyzer`，不再读取旧键。

**文件路径无法表明其所属的包与所实现的契约**，因此没有可迁移的内容。提示本身只能告知「以前选择的文件
已不再使用」，其效果与新键缺失、读出为空、即「尚未选择」相同，而后者不需要一段只保留一个版本的代码。

**不要**将裸 .onnx 自动包装为包，否则相当于永久保留第二个入口。这一决定仍然有效。

设置页另有一项落地时增加的行为：**引擎中找不到的引用不从列表中移除**，而是以「(not installed)」后缀保留
在原位置。否则该项会静默变为列表中的第一项，使用户误以为选择未变。

## 7. 时间轴换算

时间轴换算是迁移所修复的主要缺陷。otter 一律输出绝对秒，lite 使用自己的 `Timeline` 逐点换算，而非
整曲统一换算。

**音高**（`ExtractPitchTask::processOutput` 的重写）：

```cpp
// 帧间隔取自声明，不再硬编码 10ms。
const double intervalMs = schema.interval * 1000.0;
for (i) {
    sourcePositions[i] = result.startTime * 1000.0 + i * intervalMs;
}
// 其余与迁移前相同：按 5 tick 网格取重叠区间，MathUtils::resample 到目标位置。
```

otter 的结果锚定在宿主传入的 `AudioSegment::startTime` 上，该值所属的时间轴由宿主决定。lite 的实际做法
（`ExtractPitchTask` / `ExtractMidiTask`）是传入**音频文件内的相对秒**（切片在文件中的起点），取得结果后
再加上 `m_input.audioMaterialOriginMs`，换算到工程时间轴：

```cpp
sourcePositions[i] = m_input.audioMaterialOriginMs + curve.startTime * 1000.0 + i * intervalMs;
// 音符同理：startMs = m_input.audioMaterialOriginMs + note.start * 1000.0
```

因此旧实现中按帧号推算的 `frameOffsetMs` 不再需要：片内偏移由 otter 按 `startTime` 给出，文件在工程中的
位置由 lite 加上。

**音符**：

```cpp
for (const auto &note : result.notes) {
    const auto startMs = m_input.audioMaterialOriginMs + note.start * 1000.0;
    const auto endMs = startMs + note.duration * 1000.0;
    const auto startTick = timeline.msToTick(startMs) - m_input.audioClipStartTick;
    const auto endTick = timeline.msToTick(endMs) - m_input.audioClipStartTick;
    out.push_back({note.key, qRound(startTick), qRound(endTick - startTick)});
}
```

起点与终点分别换算后相减，而不单独换算时长，因为变速曲中同一时长在不同位置对应的 tick 数不同，这正是
迁移前缺陷的成因。

**不要将 `tempo` 传给 otter。** 契约中没有该字段，也不应有。

## 8. 任务流程

改写后的 `ExtractPitchTask::runTask()` 流程如下：

```
1. engine.findAnalyzer(settings.pitchAnalyzer) → AnalyzerEntry，找不到时报告「未配置分析器」
2. spec = package.contribution(srt::InferenceCategory::NAME, id)
3. schema = spec->exports()->as<F0Schema>()    → sampleRate、interval、maxSegmentDuration、旋钮域
4. analyzer = F0Api::createAnalyzer(*spec->as<srt::InferenceSpec>())
5. audio = prepare(io, visibleStartMs, visibleEndMs, schema.sampleRate, cancelled, error)
6. slices = SlicingProfile::slice(audio, schema.maxSegmentDuration)
7. 逐片:
     input.audio = { schema.sampleRate, audio.channelCount, slice.samples, slice.startMs / 1000.0 }
     // 旋钮一律不填，与迁移前的行为一致；模块使用自己的默认值
     input.progress = [&](double p) { setStatus(...(done + p) / total...); }
     result = analyzer->start(input)
     处理 result，累积
8. analyzer 在 Task 析构前销毁；包句柄由 SynthrtEngine 持有
```

`ExtractMidiTask` 结构相同，`NoteStartInput` 多几个旋钮，`knownNotes` 本轮留空。

### 取消

迁移前 `ExtractTask::terminate()` 持有 `m_extractor` 并调用其 `terminate()`；迁移后改为持有
`otter::AnalysisExecutive*` 并调用 `stop()`，语义相同。片间循环也检查取消标志，因此一次取消最多等待一片
的处理时间。

被停止的执行不返回部分结果：`start()` 返回错误码为 `otter::AnalysisError::Cancelled` 的错误，`state()`
为 `Canceled`。宿主按错误码区分取消与失败。执行体已产出结果或已因其他原因失败之后才到达的停止请求，
不改变既有的结果与状态（A28）。这一点与 wolf linguist 停止后仍返回已完成部分的行为不同，属有意设计。

### 进度

otter 的进度在每次执行内从 0 到 1。lite 将其折算进「当前片 / 总片数」，由于各片长度不等，这比迁移前
按帧数折算更准确。

## 9. 落地情况

六个阶段均已完成，落地时 lite 全树编译通过，73 个测试中 72 个通过。

| 阶段 | 内容 | 结果 |
| :-- | :-- | :-- |
| **L1** | `AudioSlicer` 与 `AnalysisAudio`，新增 `TestAudioSlicer` | 完成，先于迁移落地（见下） |
| **L2** | `SynthrtEngine` 列举与创建 otter 的分析器 | 完成。`analyzers(interface)` / `createAnalyzer(reference)`，后者返回 `AnalyzerLease`（PackageHandle + 声明指针 + executive，成员顺序保证 executive 先于包释放），抽参任务从租约读取 `exports` |
| **L3** | `ExtractPitchTask` 改写 | 完成 |
| **L4** | `ExtractMidiTask` 改写 | 完成 |
| **L5** | 设置界面 | 完成，两个下拉框；旧设置不迁移，理由见 §6 |
| **L6** | `ExtractionAutomationAdapter` | 完成。校验由「文件是否存在」改为「引用所指的分析器是否已安装、是否实现该契约」 |

**L1 先于迁移落地**：L1 与 synthrt 完全无关，改动为 4 个新文件与 1 个测试目录，未修改既有代码。
已验证的行为：三段有声、两段静音的输入切成 3 片，切点位于静音段内；短于 `minimumLength` 的整段保留；
25 秒无静音的输入在 10 秒上限下切成 3 等份，无间隙、无碎片；**talcs 在
`AudioFormatInputSource::open(bufferSize, rate)` 处重采样**（44.1 kHz 文件按 16 kHz 请求，读回恰好
32000 个样本），该行为已由测试用例覆盖；`startMs` 报告实际起始帧而非请求值，两者在请求被裁剪时不同。

## 10. 真实模型验证

**GAME 已验证。** GAME 1.0.3 small 模型打包为 `otter/game`，三个合成音 A3 / C4 / E4 转录为
MIDI 57 / 60 / 64，边界位置、时长与占空比均与输入吻合。

打包形状如下，为 `otter-game` 0.2.0.0 包中实际发布的声明（`inferences/note/inference.json`，随
models-v0.3.0.0 发布）。与方案期草案相比有两处修改：填写了 `maxSegmentDuration`，因为缺省上限会使宿主
只能猜测；语言表只保留宿主词汇，模型编号移入 `configuration`。

```json
{ "interface": "org.openvpi.otter.inference.Note", "level": 1, "variant": "game",
  "exports": {
    "sampleRate": 44100, "channelCount": 1, "maxSegmentDuration": 60,
    "languages": [ "cmn", "eng", "jpn", "yue" ], "defaultLanguage": "cmn",
    "supportsKnownNotes": true,
    "knobs": { "boundaryThreshold": { "minimum": 0, "maximum": 1, "default": 0.2 },
               "boundaryRadius": { "minimum": 0, "maximum": 1, "default": 0.02 },
               "noteThreshold": { "minimum": 0, "maximum": 1, "default": 0.2 },
               "notePresenceCutoff": { "minimum": 0, "maximum": 1, "default": 0.5 },
               "steps": { "minimum": 1, "maximum": 64, "default": 8 } } },
  "configuration": {
    "encoder": "../../encoder.onnx", "segmenter": "../../segmenter.onnx",
    "estimator": "../../estimator.onnx",
    "boundaryToDuration": "../../bd2dur.onnx", "durationToBoundary": "../../dur2bd.onnx",
    "timestep": 0.01,
    "languages": { "eng": 1, "jpn": 2, "yue": 3, "cmn": 4 } } }
```

`configuration` 的路径以 `../../` 开头：模型位于包根，声明位于 `inferences/note/` 下，而规范 2.4 规定
相对路径以**声明文件所在目录**为基准。**RMVPE 已按相同形状打包**，0.2.0.0 包随 models-v0.3.0.0 发布，
数值验证尚未进行。

**验证中修正的两处实现**：真实导出模型的 `t` 是**批次维**，采样循环位于模型外部；解释器原先将整条
schedule 一次传入，任何真实模型都无法运行。此外，三个旋钮是标量，`presence` 输出是布尔值。fixture 原先
按解释器的假设声明形状，因此测试始终通过而未能发现这些问题。

## 11. 未决

- **`minLength` 取 2 秒是否对 rmvpe 有损。** 原参数为 5 秒。切得更细会使静音段更早被切开，理论上不影响
  逐帧基频估计，但现已有真实模型，应实测一次。
- **跨片边界。** 早先此处记载「GAME 的 segmenter 有 `prev_boundaries` 输入，可由上一片的结果填充以保持
  连续」。**这是误读，已由真实模型证伪**：`prev_boundaries` 是**采样循环自身的状态**，每一步将上一步的
  输出回送，与上一片音频的边界无关。该模型没有现成的跨片连续性入口。
- **两个 Task 的样板重复。** L3/L4 完成后，`ExtractPitchTask` 与 `ExtractMidiTask` 的结构高度相似
  （获取分析器、读取 schema、准备音频、切片、逐片执行、换算到时间轴）。原先约定「若出现重复则下沉到
  otter」，该条件已满足，但下沉前须明确下沉的范围：两者的差别在于结果形状（曲线与音符）与时间轴换算方式，
  共同部分只有前半段。
- **`ExtractPitchTask` / `ExtractMidiTask` 尚未端到端运行。** 其下的分析器层已用真实模型运行；两个 Task
  需要一个工程与一个音频剪辑才能验证。
