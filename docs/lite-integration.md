# otter 接入 ds-editor-lite（M6）

本文是 otter 在 lite 侧落地的方案。它只写 lite 这一侧的改动；otter 自身的分层与契约见
[otter-design.md](otter-design.md)，冲突时以那一份为准。

## 前置条件

M6 依赖 lite 迁到 **synthrt main + wolf**。原因不是偏好而是硬约束：otter 的类别注册在
`srt::ContribCategoryRegistry` 上，而 lite 今天链接的是 synthrt 的 refactor 线，两条线的
`SynthUnit` 不是同一个类型。在 lite 完成那次迁移之前，本文的任何一步都无处可放。

这也意味着本文与 lite 的迁移分支合流，不单独开分支。

## 锚点

| 树 | 观察点 |
| :-- | :-- |
| lite | `origin/main` `a8fac646` |
| lite 历史（切片器的出处） | `f807eaea^`，即删除 `audio-util` / `game-infer` / `rmvpe-infer` 之前 |
| otter | `analysis-level-1` |
| talcs | vcpkg `talcs_x64-linux`，`TalcsFormat` / `TalcsCore` |

---

## 1. lite 今天怎么抽参

```
ExtractPitchTask::runTask()
  ├─ options->general()->rmvpePath           用户在设置里选的一个裸 .onnx 文件
  ├─ runtime.services().get<PluginFactory>() refactor 线的插件工厂
  ├─ plugins->plugin<PitchExtractorPlugin>("rmvpe")
  ├─ ExtractorUtils::decodeAudio()           srt::audio::AudioPipeline（ffmpeg 解码 + 重采样）
  ├─ extractor->extract(buffer, sampleRate)  插件内部再重采样一次并 RMS 切片
  └─ processOutput()                         freqToMidi、按 10ms 硬编码间隔重采样到 5 tick 网格
```

`ExtractMidiTask` 同形，多一个 `options.tempo = timeline.tempoAt(0)`。

这条路径上有三个已知缺陷，迁移会一并修掉：

- **变速曲的音符位置系统性偏移。** `tempoAt(0)` 只取第一个速度标记，而插件用它把秒换算成 480 PPQ
  的 tick。曲子中途变速，之后的每个音符都错。
- **`intervalMs = 10.0` 写死在 lite 里。** 换一个帧率不同的音高算法，lite 不会知道。
- **对齐能力用不上。** GAME 的权重支持在已知音符上做条件，但 refactor 的封装把那条 session 丢了。

## 2. 目标形状：谁负责什么

| 事项 | 今天 | 迁移后 |
| :-- | :-- | :-- |
| 解码 | `srt::audio::AudioPipeline`（ffmpeg） | talcs `AudioFormatIO` |
| 重采样 | 插件内部 | talcs `AudioFormatInputSource::open(bufferSize, rate)`，rate 来自模块声明 |
| 静音切片 | 插件内部 | lite，一套统一参数 |
| 帧间隔 | lite 写死 10ms | 从 `F0Schema::interval` 读 |
| 秒 → tick | 插件内部，单一 tempo | lite 的 `Timeline`，逐点换算 |
| 模型选择 | 设置里选 .onnx 文件路径 | 选一个已安装的抽参器贡献 |
| 参数 | 无 UI，全用模型默认 | 不变，仍全用模型默认（见 §6） |

### 关于音频格式覆盖面

迁移后解码走 talcs，而 lite 只注册了 `StandardFormatEntry`（libsndfile）与 `WavpackFormatEntry`，
**没有 ffmpeg entry**。表面上看比现在的 ffmpeg 窄。

实际上不窄：`ExtractTask::Input::audioPath` 指向的是**工程里已经导入的音频剪辑**，那个文件此前必须
先过 `FormatManager` 才进得了工程。所以抽参能看到的文件，talcs 本来就能读。今天的 ffmpeg 解码是这
条路径上多出来的一份能力，没有可达的输入用得上它。

（如果以后要支持直接对工程外的任意文件抽参，那是给 `FormatManager` 加一个 entry 的事，与 otter 无关。）

## 3. 文件落点

改动面在复核时收窄过一轮。原稿要新建一个 `src/libs/AudioAnalysis` 库和一个
`src/app/Modules/Analysis` 模块目录，两者都取消了：切片器只有抽参用得上，lite 的测试约定本来就是
直接重编 app 源文件，不需要库目标；而 `SynthrtEngine` 已经是 `SynthUnit` 的门面，列举抽参器是它加两
个方法的事，不值得另开模块。原稿还要按 `Schema` 自动生成旋钮 UI，那是独立需求，不该绑在迁移里 ——
迁移的最小形态是继续不传旋钮、全用模块默认值，与今天的行为一致。

**新增**（全部在 `src/app/Modules/Extractors/` 之下，不新建库、不新建模块目录）

```
AudioSlicer.{h,cpp}      RMS 静音切片 + 超长片硬性等分。算法取自 f807eaea^ 的
                         src/libs/audio-util，参数改为按时间表述
AnalysisAudio.{h,cpp}    talcs 解码 + 重采样到抽参器声明的采样率，产出单声道
```

**改写**

```
ExtractPitchTask.{h,cpp}
ExtractMidiTask.{h,cpp}
src/libs/SynthrtEngine/SynthrtEngine.{h,cpp}        加 analysis 类别的插件路径与两个查询方法
src/app/UI/Dialogs/Options/Pages/GeneralPage.cpp    两个 FileSelector → 两个 QComboBox
src/app/Model/AppOptions/Options/GeneralOption.h    rmvpePath 改存贡献引用（字段类型不变）
src/app/Automation/ExtractionAutomationAdapter.cpp  modelId 解析走目录而不是路径
```

**删除**

```
ExtractorUtils.{h,cpp}   只做 ffmpeg 解码，被 AnalysisAudio 取代
```

`ExtractorUtils` 到两个 Task 改写时才删。在那之前它与新文件并存 —— 删它会连带改两个 Task，而两个
Task 依赖 refactor 线，那样这一步就无法在今天验证。

## 4. 音频准备

一个薄适配器，把 talcs 的读取变成 otter 要的那一段 PCM（`Modules/Extractors/AnalysisAudio.h`）：

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

实现是 `AudioFormatInputSource(io)` + `open(chunk, sampleRate)` + `setNextReadPosition` + 循环读。
**重采样发生在 `open()`**：这原本是个假设，现在由 `TestAudioSlicer` 证实 —— 44.1 kHz 的文件按
16 kHz 请求，读回恰好 32000 个样本（2 秒）。

产出**单声道**而不是交错多声道。talcs 的缓冲本来就是分平面的，而所有已有抽参器都要单声道、otter 也
会自己平均，在读的时候顺手平均既省掉一次交错、又少搬一半字节。契约两种都接受。

`startMs` 报的是**实际读到的起始帧**而不是请求值。两者在请求被裁剪时不同，而抽参器把结果锚定在这个
数上，差值会原样变成结果里的位移。

**校验交给 otter。** 采样率不匹配、样本数不是整帧、超过 `maxSegmentDuration`，执行体都会报错并说明
原因。lite 不重复一遍，重复会分叉。

## 5. 切片

切片算法从 `f807eaea^:src/libs/audio-util/src/Slicer.cpp` 取回，落在
`Modules/Extractors/AudioSlicer.h`，命名空间 `Extractors`。边界处理取 synthrt refactor 版加固过的
写法（空区间的 `argmin` 返回 0 而不是断言，参数非负钳位），不用更早那版带 `assert` 的。

参数统一成一套，不按算法分。理由是换算成时间之后两套本来就几乎一样：

| 参数 | rmvpe（16k） | game（44.1k） | 统一取值 |
| :-- | :-- | :-- | :-- |
| threshold | 0.02 | 0.02 | 0.02 |
| hopSize | 160 = 10ms | 441 = 10ms | 10ms |
| winSize | 640 = 40ms | 1764 = 40ms | 40ms |
| minLength | 500 帧 = 5s | 200 帧 = 2s | **2s** |
| minInterval | 30 帧 = 300ms | 30 帧 = 300ms | 300ms |
| maxSilKept | 50 帧 = 500ms | 50 帧 = 500ms | 500ms |

`minLength` 取两者中较小的 2 秒：切得细一点对 rmvpe 无害，而 GAME 有 60 秒上限。

`SlicingProfile` 把这些存成时间，按目标采样率现算成帧，所以同一套配置对两个采样率都对。

**切片器必须保证每片不超过 `Schema::maxSegmentDuration`。** RMS 切片在一段持续有声的长音频上可能
切不出足够短的片，所以在静音切片之后还有一道硬性再分。这一道不能省 —— GAME 对超长片是直接报错，
不是降级。

再分按**等份**而不是「切满上限 + 余数」：25 秒按 10 秒上限分成 3 份每份 8.33 秒，而不是 10 + 10 + 5。
末尾的碎片是比三份均等更差的输入。片的端点由两端反算而非累加，所以相邻片之间既不留缝也不重叠。

不做重叠。重叠要求宿主知道模型如何合并交叠区域，那是模型的事；`prev_boundaries` 那条路（otter 的
未决项）才是跨片连续性该走的地方。

## 6. 模型发现与设置界面

### 包的安装位置

抽参模型包与声库包**共用同一个 `SynthUnit` 与同一组搜索路径**（otter A13）。`SynthrtEngine` 在
初始化时多做两件事：

```cpp
// analysis 类别的解释器插件在哪
unit.setPluginPaths(otter::ANALYSIS_CATEGORY, {pluginRoot / "plugins/otter/analysisproviders"});
// 包路径与声库共用，已有
```

`SynthrtEngine` 在包加载完成后能列举出来 —— 两个方法，不另开模块目录：

```cpp
struct AnalyzerEntry {
    std::string packageId;          // "openvpi/game"
    stdc::VersionNumber packageVersion;
    std::string contributionId;     // "note"
    std::string interfaceName;      // org.openvpi.otter.analysis.F0 / .Note
    std::string variant;
    srt::DisplayText name;          // 声明里的 name，多语言

    std::string reference() const;  // "openvpi/game:analysis/note"，持久化用这个
};
```

`reference` 是设置里存的东西 —— 不是文件路径。文件路径会随安装位置变，贡献引用不会。

### 装错根的表现（实测，供安装方对照）

包放错目录时宿主**不会报错**，只是分析器不上榜——两个状态分得很开，别把它们读成一个：

| 现象 | 含义 | 判据（lite 侧） |
| :-- | :-- | :-- |
| `module_state = "unavailable"` | 没有任何包应答该契约 | `SynthrtEngine::analyzers(interface)` 为空 |
| `module_state = "ready"` 但 `available = false`，理由 "…model is not configured" | 契约有实现者了，只是**人还没在设置里选** | `settings.general.pitchAnalyzer` / `noteAnalyzer` 为空 |
| 两者都过 | 可跑 | `available = true` |

易踩的两点：

1. **`wolf/packages` 不是抽参包的安装根**。它是 wolf 语言包的**依赖查找**根：lite 把它作为 `packagePaths`
   交给 Bootstrap（供"某个声库点名依赖某语言包"时查找），而**扫描根**是用户设置的包搜索路径
   （`SynthrtEngine` 的 `voicebankPaths` 实参，`InferEngine.cpp:207-217` 的绑定），`analyzers()` 读的正是
   本次扫描的结果（"the analysers the last scan found"，`SynthrtEngine.h:158`）。放错根的现象就是上表
   第一行，而且**日志里确实无痕**——包根本没进扫描列表，自然不会有人去打开它，也就没有可报的失败。
2. **`packages.list` 列的是声库目录里的包**，分析器包不会出现在这份清单里。要确认分析器是否上榜，
   问 `extract.get_capabilities`，别看包清单。

### 声明不合格时：运行期拒载长什么样（实测）

包被扫到了、但声明违反契约的两条互检规则时（provider 装载期即拒，`game/main.cpp:747-750` 与 `:743`；linter
的 `scripts/check-declarations.py:279-285` 是同一对规则，**打包期就该挡住**）：

```
SynthrtEngine: could not open D:\...\otter-game@0.1.0.0 : failed to interpret module exports:
    the exports declare supportsKnownNotes but the configuration names no durationToBoundary model
```

- 宿主侧可观测面：该契约 `module_state = "unavailable"`、`available = false`（reason `… module is
  unavailable`），**另一路不受牵连**（同一次实测里 pitch 仍 `ready`/`true`）；
- 硬发命令会得到明确拒绝：`The note analyzer is not installed: otter/game:analysis/note`
  （`file_not_found`，字段 `note_analyzer`）；
- ⇒ 与本文件前面那句"日志里无痕"并不矛盾：**没被扫描到**才无痕，**扫到但声明坏**是有痕的，且会指名包与原因。

2026-09 实测链路（lite 隔离副本 + 本仓 `packages/{rmvpe,game}`，模型按声明路径就位）：
`module_state` 两路都 `ready`；选中 `otter/rmvpe:analysis/f0` 与 `otter/game:analysis/note` 后
`available` 两路为 `true`；`extract.pitch.start` 与 `extract.midi.start` 都被接受，且 ONNX 驱动按
**包内声明路径**开出了六个模型（`otter-rmvpe@0.1.0.0/rmvpe.onnx` 与 `otter-game@0.1.0.0/{encoder,
segmenter,estimator,bd2dur,dur2bd}.onnx`）——即"声明 → 模型"这一段是通的。同一轮里任务随后停在下游：
`ExtractTask` 报 "Failed to open the audio file"（`AnalysisAudio.cpp:38-41`），与本仓无关，属 lite 侧
音频来源路径的问题，另行跟踪。

### 语言标识用**宿主的**词汇表

宿主把语言 id **逐字**递给分析器（lite: `ExtractionAutomationAdapter.cpp:128`），不做翻译——因此
分析器自己要按宿主 id 认语言，声明两侧都写宿主词汇：

| 位置 | 写什么 | lite 的取值 |
| :-- | :-- | :-- |
| `exports.languages` | 宿主 id 表（分析器接受哪些） | ISO 639-3：`cmn` `eng` `jpn` `yue`（`src/app/Global/AppGlobal.h:28`） |
| `configuration.languages` | 键=宿主 id，值=**模型自己的**编号 | 例：`{"eng":1,"jpn":2,"yue":3,"cmn":4}` |
| `configuration.defaultLanguage` | 同上，宿主 id | `cmn` |

模型之间不必同意"1 是哪种语言"，所以编号表天然属于 `configuration`；`exports` 只回答"宿主问哪种语
言时我能答"。反例（本仓 `0.1.0.0` 第一条声明即如此，已改）：写模型词汇 `zh/en/ja/yue`，宿主递 `cmn`
⇒ 查表未命中，任务以模型内抛出的 `this model does not know the language cmn` 失败，宿主原样上报。
F0 契约没有语言项（RMVPE 无此字段），不受这条影响。

### 声明的音频格式是承诺，分析器必须自己校验

`exports` 里的 `sampleRate` / `channelCount`（F0 还有 `interval`）是**"宿主会照它准备音频"的承诺**。
契约为真不等于模型能履行：**无法履行就必须在 `createExports` 里拒绝**，否则错的是数据而不是错误码。

代价实测过（lite 2026-09，同一份代码、同一段音频、只改活包声明的采样率）：

| GAME 声明 | 结果 |
| :-- | :-- |
| `44100`（模型真实值） | 594 个音符，中位音高键 64 |
| `22050`（错报） | **569 个音符，中位音高键 76（整一个八度）**，首音位置与跨度几乎不变，**日志零异常** |

即"错报声明 → 静默高八度"，在编辑器里看起来像一次正常抽参。低报采样率等于让模型按两倍速度读音频，
频率翻倍，正好 +12 半音——数字与理论吻合，说明这条不是噪声。

拒绝的写法（两个 provider 同形，`InvalidFormat` / `FeatureNotSupported`）：

```
the rmvpe variant runs at 16000 Hz; the exports declare 44100      // rmvpe: sampleRate + interval
the game variant runs at 44100 Hz; the exports declare 22050       // game: sampleRate + channelCount
```




### 设置界面

`GeneralPage` 的两个 `FileSelector` 换成两个下拉框，分别按 `interfaceName` 过滤出音高抽参器与音符
抽参器。**本轮到此为止** —— 不做旋钮 UI。

理由是范围控制：lite 今天一个抽参旋钮都没有，全部用模型默认值，迁移后 `StartInput` 的旋钮一律不填，
行为与今天完全一致。按 `Schema` 自动生成设置界面是一件独立的、有价值的事，但它不是迁移的一部分，把
它捆进来会让这次改动多出一整套 UI 代码，而那套代码的对错与迁移是否成功无关。契约那边已经准备好了
（`Knob` 带取值域与默认值），要做的时候再做。

### 旧设置不迁移（落地时改的决定）

原计划是把 `rmvpePath` 留一个版本、启动时提示一次再清掉。实际没有这么做：`rmvpePath` / `gameDir` 直接
换成 `pitchAnalyzer` / `noteAnalyzer`，旧键不再读。

**因为一个路径说不出它来自哪个包、答哪个契约**，没有能迁进去的东西——提示本身也只能说「你以前选的那个
文件现在没用了」，与旧配置里根本没有新键、读出来为空、等于「还没选」是同一个效果，而后者不需要一条
只活一个版本的代码。

**不要**试图把裸 .onnx 自动包装成一个包——那等于永久保留第二扇门。这条仍然成立。

设置页另有一条落地时加的：**引擎里找不到的引用不会从列表里消失**，而是带「(not installed)」留在原位。
否则它会静默变成列表里的第一项，人以为还是原来那个。

## 7. 时间轴换算

这是迁移真正修掉的缺陷。otter 一律输出绝对秒，lite 用自己的 `Timeline` 换算，逐点而不是逐曲。

**音高**（`ExtractPitchTask::processOutput` 的重写）：

```cpp
// 帧间隔从声明来，不再写死 10ms。
const double intervalMs = schema.interval * 1000.0;
for (i) {
    sourcePositions[i] = result.startTime * 1000.0 + i * intervalMs;
}
// 之后与今天相同：按 5 tick 网格取重叠区间，MathUtils::resample 到目标位置。
```

`result.startTime` 已经是这一片在工程时间轴上的绝对位置，所以今天那句
`m_input.audioMaterialOriginMs + frameOffsetMs` 里的 `frameOffsetMs` 不再需要 —— 由 lite 在
`prepare()` 时把 `startMs` 填对即可。

**音符**：

```cpp
for (const auto &note : result.notes) {
    const int startTick = qRound(timeline.msToTick(note.start * 1000.0));
    const int endTick   = qRound(timeline.msToTick((note.start + note.duration) * 1000.0));
    out.push_back({note.key, startTick, endTick - startTick});
}
```

先各自换算再相减，而不是把时长单独换算 —— 变速曲里同一个时长在不同位置对应的 tick 数不同，这正是
今天那个缺陷的形状。

**别把 `tempo` 传给 otter。** 契约里没有这个字段，也不该有。

## 8. 任务的形状

`ExtractPitchTask::runTask()` 之后是：

```
1. engine.findAnalyzer(settings.pitchAnalyzer) → AnalyzerEntry，找不到就报「未配置抽参器」
2. spec = package.contribution("analysis", id)
3. schema = spec->exports()->as<F0Schema>()    → sampleRate、interval、maxSegmentDuration、旋钮域
4. extension = findFromSpec<F0Executive>(*spec)
   analyzer  = extension->createAnalyzer(F0RuntimeOptions(variant))
5. audio = prepare(io, visibleStartMs, visibleEndMs, schema.sampleRate, cancelled, error)
6. slices = SlicingProfile::slice(audio, schema.maxSegmentDuration)
7. 逐片:
     input.audio = { schema.sampleRate, audio.channelCount, slice.samples, slice.startMs / 1000.0 }
     // 旋钮一律不填，与今天的行为一致；模块用自己的默认值
     input.progress = [&](double p) { setStatus(...(done + p) / total...); }
     result = analyzer->start(input)
     处理 result，累积
8. analyzer 在 Task 析构前销毁；包句柄由 SynthrtEngine 持有
```

`ExtractMidiTask` 同形，`NoteStartInput` 多几个旋钮，`knownNotes` 本轮留空。

### 取消

`ExtractTask::terminate()` 今天持一个 `m_extractor` 并调 `terminate()`。改成持
`otter::AnalysisExecutive*` 并调 `stop()`；语义一致。片间循环也要检查取消，这样一次取消最多等一片。

### 进度

otter 的进度是每次执行 0→1。lite 把它折算进「第几片 / 共几片」，比今天按帧数折算更准，因为片长不等。

## 9. 落地情况

六个阶段都已完成，lite 的整棵树编译通过，72/73 测试通过。

| 阶段 | 内容 | 结果 |
| :-- | :-- | :-- |
| **L1** | `AudioSlicer` 与 `AnalysisAudio`，加 `TestAudioSlicer` | 完成，先于迁移落地（见下） |
| **L2** | `SynthrtEngine` 托管 analysis 类别，列举与创建 | 完成。`analyzers(interface)` / `createAnalyzer(reference)`，后者返回 `AnalyzerLease`（PackageHandle + 声明指针 + executive，析构顺序保证 executive 先于包释放），抽参任务从租约上读 `exports` |
| **L3** | `ExtractPitchTask` 改写 | 完成 |
| **L4** | `ExtractMidiTask` 改写 | 完成 |
| **L5** | 设置界面 | 完成，两个下拉框；旧设置不迁移，理由见 §6 |
| **L6** | `ExtractionAutomationAdapter` | 完成。校验从「文件存不存在」改为「引用指的分析器装没装、答不答这个契约」 |

**L1 先于迁移落地**：它与 synthrt 完全无关，改动面是 4 个新文件加 1 个测试目录，既有代码一行未改。
验证到的：三段有声两段静音切成 3 片且切点落在静音段内；短于 `minimumLength` 的整段保留；25 秒无静音
在 10 秒上限下切成 3 等份无缝隙无碎片；**talcs 确实在 `AudioFormatInputSource::open(bufferSize, rate)`
处重采样**（44.1 kHz 文件按 16 kHz 请求读回恰好 32000 个样本）——这原本是个假设，现在是一条用例；
`startMs` 报的是实际起始帧而非请求值，两者在请求被裁剪时不同。

## 10. 真实模型验证

**GAME 已验证**。GAME 1.0.3 small 模型打成 `openvpi/game` 包，三个合成
音 A3 / C4 / E4 转录回 MIDI 57 / 60 / 64，边界落在秒上，时长与占空比吻合。

打包形状（`maxSegmentDuration` 故意不填——不知道这个模型的真实上限，填一个就是编造；为 0 即不限，
宿主仍按静音切段）：

```json
{ "interface": "org.openvpi.otter.analysis.Note", "level": 1, "variant": "game",
  "exports": {
    "sampleRate": 44100, "channelCount": 1,
    "languages": [ "cmn", "zh", "eng", "en", "jpn", "ja", "yue" ],
    "supportsKnownNotes": true,
    "knobs": { "boundaryThreshold": { "minimum": 0, "maximum": 1, "default": 0.2 },
               "boundaryRadius": { "minimum": 0, "maximum": 1, "default": 0.02 },
               "noteThreshold": { "minimum": 0, "maximum": 1, "default": 0.2 },
               "notePresenceCutoff": { "minimum": 0, "maximum": 1, "default": 0.5 },
               "steps": { "minimum": 1, "maximum": 1000, "default": 8 } } },
  "configuration": {
    "encoder": "./encoder.onnx", "segmenter": "./segmenter.onnx",
    "estimator": "./estimator.onnx",
    "boundaryToDuration": "./bd2dur.onnx", "durationToBoundary": "./dur2bd.onnx",
    "timestep": 0.01,
    "languages": { "cmn": 4, "zh": 4, "eng": 1, "en": 1, "jpn": 2, "ja": 2, "yue": 3 } } }
```

语言表把生态写法与模型自己的写法都映射过去。**RMVPE 尚未打包**，模型到齐后按同样形状处理。

**验证时改掉的两件事**（见 otter 提交「Drive the GAME models the way a real export declares」）：
真实导出的 `t` 是**批次维**，采样循环在模型外面——provider 原先把整条 schedule 一次传进去，任何真实
模型都跑不了；三个旋钮是标量，`presence` 输出是布尔。fixture 原先照着 provider 的假设声明形状，所以
测试永远绿。

## 11. 未决

- **`minLength` 取 2 秒是否对 rmvpe 有损。** 原参数是 5 秒。切得细会让静音段更早被切开，理论上对逐帧
  基频估计无影响，但现在有真实 GAME 模型了，值得量一次。
- **跨片边界。** 早先这里写着「GAME 的 segmenter 有一个 `prev_boundaries` 输入，本可由上一片的结果
  填充以保持连续」。**这是误读，已由真实模型证伪**：`prev_boundaries` 是**采样循环自己的状态**——每一步
  把上一步的输出喂回去——而不是上一片音频的边界。跨片连续性在这个模型里没有现成的入口。
- **两个 Task 的样板重复。** L3/L4 写完后，`ExtractPitchTask` 与 `ExtractMidiTask` 的结构确实高度雷同
  （取分析器、读 schema、备音频、切片、逐片跑、落回时间轴）。当初说「若重复再把它下沉到 otter」，现在
  条件满足了，但下沉之前要想清楚下沉的是什么：两者的差别在结果形状（曲线 vs 音符）与时间轴换算方式，
  共同的只有前半段。
- **`ExtractPitchTask` / `ExtractMidiTask` 本身未端到端跑过。** 其下的分析器层已用真实模型实跑；两个
  Task 需要一个工程与一个音频剪辑才能验。
