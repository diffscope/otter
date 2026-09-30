# HFA 接入 otter：Align 契约、`otter/hfa` 包与发布（V1）

> **状态：本方案已实施。** 本文保留为方案与实施记录；决策已并入 [`docs/otter-design.md`](../otter-design.md)
> 的台账（A19–A25）。此后的变更以 otter-design.md 为准，主要有：
> - A26：分析器改为 synthrt 内置 `inference` 类别下的模块，接口名为 `org.openvpi.otter.inference.{F0,Note,Align}`，
>   执行体派生 `srt::InferenceExecutive`，扩展 ID 与 `AnalysisSpec` 已删除；解释器位于
>   `src/plugins/inferenceinterpreters/hfa`；包声明位于 `inferences/align/inference.json`。
> - A27：Align 的 `exports.languages` 每项为 `{language, scheme, lyrics, phonemes}`；`defaultLanguage` 与
>   `defaultNonSpeechPhonemes` 从 `configuration` 移至 `exports`。
> - 发布：`otter-rmvpe`、`otter-game`、`otter-hfa` 当前为 0.2.0.0（`inferences/` 布局），由 release
>   `models-v0.3.0.0` 提供（`manifest.json` 与三个 zip，由 `scripts/make-package.py` 生成）。本方案执行时的
>   `models-v0.2.0.0` 与此前的 `models-v0.1.0.0` 装的是旧布局的 0.1.0.0 包，当前 otter 不能载入。
>
> §2–§8 保留方案制定时的原文，其中 `org.openvpi.otter.analysis.*`、`src/plugins/analysisproviders/*`、
> `analyzers/*/analysis.json` 等名称为 A26 之前的旧名，文中在涉及处标注现名。

**范围**：为 otter 增加第三个出厂变体 `hfa`（HubertFA 强制对齐），并为其建立 `Align` 契约、打包模型、发布。
**锚点**：otter `analysis-level-1`（方案制定时的版本）；dataset-tools 工作树（只读参考）；HubertFA `v0.0.7`。

---

## 0. 结论

HFA 是**强制对齐器**：输入是「音频 + 已知歌词文本」，输出是**音素/词的时间区间**。它既不是 F0（基频曲线）
也不是 Note（音符区间），输入与输出都与两者根本不同。按规范 2.4「输入输出根本不同时另立 `interface`」，
必须新立契约。otter 已为它预留了名字与类别位置（`docs/otter-design.md` §1 表：`Align`，音素/词的时间区间；
方案制定时名为 `org.openvpi.otter.analysis.Align`，现为 `org.openvpi.otter.inference.Align`），并把契约面
列为未决项；该未决项现已由台账 A19 与 §5.4 落地。本方案的内容：

1. **立契约**：`Align` Level 1（Schema / StartInput / Result / Executive / 读取函数 / JSON Schema / lint 规则）。
2. **移植变体**：将 dataset-tools 的 C++ HFA 推理移植为解释器插件（方案制定时位于
   `src/plugins/analysisproviders/hfa`，现位于 `src/plugins/inferenceinterpreters/hfa`），外壳按 game/rmvpe 的结构
   重写，算法逐行保留，以保证与参考实现数值可比。
3. **打包**：`packages/hfa` 声明 + HubertFA `v0.0.7` 模型 → `otter-hfa-0.1.0.0.zip`（现为 0.2.0.0）。
4. **发布**：按已发布的 `models-v0.1.0.0` 的形状（zip + `manifest.json`）发布新版本（发布形态见 D2）。
5. **验证**：fixture 单测 + 真实权重 + **与 dataset-tools `dstools-cli hfa` 的逐音素数值比对**（同算法、同输入）。

---

## 1. 决策点

| 编号 | 决策点 | 推荐 | 备选与取舍 |
| :-- | :-- | :-- | :-- |
| **D1** | 是否新立 `Align` L1 契约 | **是**（HFA 只能落在此处） | 并入 `Note`（knownNotes）：输入需要文本，输出需要音素，`Note` 均无法容纳，否决 |
| **D2** | 发布形态 | **新 tag `models-v0.2.0.0`，三包齐全 + 全量 `manifest.json`**（rmvpe/game 从旧 release 原样搬运，SHA512 复核） | ①只发 `otter-hfa` 增量包（tag `models-v0.1.1.0`）：上传量小，但一个 bundle 拆成两个 release，安装方需要拼合；②覆盖旧 tag：**禁止**（已发布资产不可变） |
| **D3** | 授权范围 | **下载 v0.0.7 模型 zip（256 MB）＋ 本地重建 otter＋测试 ＋ 创建 GitHub release** | 分开授权：只做前两步，发布另行授权 |
| **D4** | 歌词与静音词的契约形态 | `AlignStartInput::lyrics` 为单个字符串；结果**覆盖整段**，静音段以 `exports.silenceLabel`（默认 `"SP"`）为文本出现 | ①歌词改为 `vector<string>` 词表：模型本身按空白切分，多一层无收益；②结果只给出对齐到的词，静音由宿主计算：少一层词汇，但失去模型自带的分区（`fill_small_gaps` 会调整词的边界） |
| **D5** | 旋钮集合 | 三个：`nonSpeechThreshold`(0–1, 0.5)、`nonSpeechMinDuration`(s, 0.1)、`gapFill`(s, 0.1) | 不提供旋钮：用户无法调节呼吸段灵敏度；提供更多（Viterbi 相关）：属模型内部参数，不进契约 |
| **D6** | 本轮是否同时实现 lite（ds-editor-lite）的 Align 接入 | **否**（本轮只含 otter 侧、包与发布） | 同轮接入：lite 需要新的 UI 与任务（歌词来源、结果所在的层），工作量远大于本轮 |

---

## 2. 方案制定时的事实

以下为方案制定时的只读调查结果。otter 侧路径为 A26 之前的布局，行号已不再对应当前代码，故只保留文件与符号。

### 2.1 otter 侧

| 事实 | 证据 |
| :-- | :-- |
| `Align` 只登记了名字与类别，未定义类型 | `docs/otter-design.md` §1 表与当时的 §11 未决条目（现为台账 A19） |
| lint 已把 `Align` 列为保留接口，但不在「已实现」集合中 | `scripts/check-declarations.py` |
| 契约三件套的形状（Schema / StartInput+Result / Executive + 扩展 traits ID） | `include/otter/Api/Note/1/NoteApiL1.h` |
| 声明读取集中在库内，每份契约一个函数 | `src/lib/Api/Note/1/NoteApiL1.cpp` |
| 解释器形状（执行体 / 会话开启 / Provider / 插件 IID） | `src/plugins/analysisproviders/game/main.cpp`（现为 `src/plugins/inferenceinterpreters/game/main.cpp`） |
| **加载期互检**：exports 与 configuration 必须自洽，否则包在装载期被拒绝 | 同上，`createExports` |
| 输入校验由库完成（采样率/声道/整帧/段上限/旋钮域） | `include/otter/Analysis/AnalysisInput.h`（`prepareSamples` / `chooseKnob`） |
| 插件与测试的构建接线 | `src/plugins/analysisproviders/CMakeLists.txt`、`src/tests/auto/Analysis/CMakeLists.txt` |
| fixture 门禁：有 Python+onnx 时生成 fixture，否则用例 `DISABLED`；运行期缺件以 `SKIP_RETURN_CODE 77` 跳过 | `src/tests/auto/Analysis/CMakeLists.txt` |
| 已发布形状：`models-v0.1.0.0`＝`otter-rmvpe-0.1.0.0.zip`＋`otter-game-0.1.0.0.zip`＋`manifest.json` | GitHub release（`gh release view/download` 实测） |
| `manifest.json` 键：`bundleVersion`、`packages[].{id,file,sha512,version,compatVersion,directory,size,models{}}` | 同上，已下载原文 |

### 2.2 参考实现侧（dataset-tools 的 HFA C++ 推理，只读）

| 文件 | 行数 | 作用 | 移植处置 |
| :-- | :-- | :-- | :-- |
| `hubert-infer/include/hubert-infer/Hfa.h`、`src/Hfa.cpp` | 49 / 231 | 装配：读取 `config.json` 的 `mel_spec_config`、`vocab.json` 的 `vocab`/`silent_phonemes`/`non_lexical_phonemes`/`dictionaries`；60 s 上限；后处理链（AP 插入 → `fill_small_gaps` → `clear_language_prefix` → `add_SP` → `check`） | 逻辑保留，外壳重写（去掉 dsfw/Qt 与音频解码） |
| `src/HfaModel.h`、`src/HfaModel.cpp` | 61 / 127 | 会话封装：输入 `waveform`；输出 `ph_frame_logits[B,C,T]`、`ph_edge_logits[B,T]`、`cvnt_logits[B,C,T]` | 换用 dsinfer 的 `InferenceSession`（与 rmvpe/game 相同） |
| `include/.../AlignmentDecoder.h`、`src/AlignmentDecoder.cpp` | 137 / 544 | Viterbi（三型转移）＋ 边界小数修正（`edge_diff/2`，clip ±0.5）＋ 帧置信度 | 逐行移植，保持算术 |
| `include/.../AlignWord.h`、`src/AlignWord.cpp` | 188 / 437 | `Word`/`Phone`/`WordList`（`add_AP`、`fill_small_gaps`、`add_SP`、`check`、`clear_language_prefix`） | 移植为扁平的 `std::vector` 结构 |
| `include/.../DictionaryG2P.h`、`src/DictionaryG2P.cpp` | 36 / 91 | 词典 G2P：`word → 音素序列`，加 `SP` 边界与 `ph_idx_to_word_idx` 映射 | 保留（含「词不在词典中则跳过」的行为） |
| `include/.../NonLexicalDecoder.h`、`src/NonLexicalDecoder.cpp` | 67 / 117 | `cvnt_logits` → softmax → 阈值/间隙/最短时长 → AP/EP 区间 | 保留 |
| 调用侧参数面（语言、非语音音素、歌词来源） | — | `src/engine/adapters/infer-bridge/HubertAlignmentProcessor.cpp:37-42`、`:76-109`、`:122-136` | 映射进契约（见 §3） |

**硬编码常量（旋钮候选）**

| 值 | 位置 | 处置 |
| :-- | :-- | :-- |
| 非语音阈值 `0.5`、`max_gap=5` 帧、`mix_frames=10` 帧 | `NonLexicalDecoder.cpp:83` | → `nonSpeechThreshold`、`nonSpeechMinDuration`（秒） |
| `add_AP` 最短 `0.1 s`、`fill_small_gaps` 间隙 `0.1 s` | `AlignWord.h:157`、`:161` | → `gapFill`（秒） |
| 音频时长 `>60 s` 直接报错 | `Hfa.cpp:161-164` | → `exports.maxSegmentDuration = 60` |
| 无 | — | pad 平均（Python 版 `--pad_times/--pad_length`）**不移植**（C++ 参考同样没有；默认 `pad_times=1` 即不 pad） |
| `merged_phoneme_groups` | `vocab.json` 有此键，**C++ 参考从未读取** | 不读取，与参考实现保持一致（见 §8） |

**等价性验证的 oracle**：`D:\projects\dataset-tools\cmake-build-release\bin\dstools-cli.exe hfa --model <模型目录> --wav <音频> --lab <歌词> [--save-wav <44.1k wav>]`
→ phoneme 层 JSON `[{phone,start,end}, …]`（`HubertAlignmentProcessor.cpp:122-136`）。
测试素材：`D:\python\HubertFA\data\evaluate\linli\wavs\BaiMaRuLuHua_0.wav`（44.1 kHz 单声道 16 bit）＋同名 `.lab`（拼音词串）；
可运行的模型目录（与发布版接口相同、词典同为 ds-zh-pinyin-lite）：`D:\python\HubertFA\0715_hfa_private`、`0223_hfa_private`。

---

## 3. 契约面：`Align` Level 1（新增）

> 本节为方案草案原文。现行契约见 `include/otter/Api/Align/1/AlignApiL1.h` 与 `docs/otter-design.md` §5.4：
> 接口名为 `org.openvpi.otter.inference.Align`，执行体派生 `srt::InferenceExecutive`，下文的扩展 ID 与
> `AnalysisSpec` 已随自有类别一并删除（A26）；`languages` 的形状与默认值的归属按 A27 修改。

新增 `include/otter/Api/Align/1/AlignApiL1.h` ＋ `src/lib/Api/Align/1/AlignApiL1.cpp`（读取函数），
形状与 Note/F0 三件套严格一致：

```cpp
namespace otter::Api::Align::L1 {

    inline constexpr char API_INTERFACE[] = "org.openvpi.otter.analysis.Align";
    inline constexpr int  API_LEVEL = 1;

    struct PhoneInfo { std::string text; double start = 0; double duration = 0; };

    /// 一个词：text 为歌词中给出的词，或模块为自行插入的段给出的标签
    /// （静音＝exports.silenceLabel；呼吸/换气＝nonSpeechPhonemes 中的标签）。
    struct WordInfo { std::string text; double start = 0; double duration = 0;
                      std::vector<PhoneInfo> phones; };

    class AlignSchema : public srt::ContribExports {
        int sampleRate = 0;                  // 契约事实，宿主按它准备音频
        int channelCount = 1;
        double maxSegmentDuration = 0;       // hfa：60
        std::vector<std::string> languages;  // 宿主词汇（cmn/eng/jpn/…）
        std::vector<std::string> nonSpeechPhonemes;  // 本模块能产出的非语音标签（hfa: AP/EP）
        std::string silenceLabel = "SP";     // 静音段在结果中的文本
        Common::L1::Knob nonSpeechThreshold;     // 0–1
        Common::L1::Knob nonSpeechMinDuration;   // 秒
        Common::L1::Knob gapFill;                // 秒
    };

    OTTER_EXPORT srt::Expected<std::unique_ptr<AlignSchema>>
        readAlignSchema(const srt::ContribSpec &spec, std::string variant);

    class AlignRuntimeOptions : public otter::AnalysisRuntimeOptions { /* 壳 */ };

    class AlignStartInput : public srt::TaskStartInput {
        Common::L1::AudioSegment audio;
        Common::L1::ProgressCallback progress;
        std::optional<std::string> language;             // 必须是 exports.languages 之一
        std::string lyrics;                              // 必填，为空即拒绝
        std::vector<std::string> nonSpeechPhonemes;      // 为空＝模块默认；非空即采用给定集合
        std::optional<double> nonSpeechThreshold;
        std::optional<double> nonSpeechMinDuration;
        std::optional<double> gapFill;
    };

    class AlignResult : public srt::TaskResult {
        std::vector<WordInfo> words;   // 起点升序、覆盖整段、相邻不重叠不留缝
    };

    class AlignExecutive : public otter::AnalysisExecutive { start / startAsync / state / stop / waitForFinished };
}

// 扩展 ID
template <> struct srt::ContribSpecExtensionTraits<otter::AnalysisSpec, otter::Api::Align::L1::AlignExecutive> {
    inline static constexpr char ID[] = "org.openvpi.otter.extension.Align";
};
```

**语义要点**（写入头文件注释，与既有契约口径一致）

- 时间一律为**绝对秒**，与 `AudioSegment::startTime` 同一基准，与 Note 契约一致。
- 结果**覆盖整段**：`words[0].start` 与最后一段的终点分别贴合 `[0, 音频时长]` 的两端，相邻词首尾相接。
  这是对齐器输出的固有形态（参考实现由 `add_SP` 保证），也免去宿主推算静音段。
- `lyrics` 的形态由**变体自行约定**（hfa：空白分隔的词/拼音音节，取自模型词典），契约只要求非空字符串。
- 旋钮一律为 `std::optional`：不填即采用模块默认值，越界即拒绝（`otter::chooseKnob`）。
- 错误条件沿用既有错误类别（`otter::AnalysisError::Cancelled` / 框架 `InvalidFormat` / `FeatureNotSupported` / `InvalidArgument`）。

---

## 4. 变体 `hfa`

### 4.1 configuration 键（变体私有，宿主不读取）

| 键 | 必填 | 含义 |
| :-- | :-- | :-- |
| `model` | ✅ | `model.onnx` 路径 |
| `config` | ✅ | 模型自带的 `config.json`（读取 `mel_spec_config.sample_rate` / `hop_size`） |
| `vocab` | ✅ | 模型自带的 `vocab.json`（读取 `vocab`、`silent_phonemes`、`non_lexical_phonemes`、`dictionaries`） |
| `languages` | ✅ | 宿主语言 id → **模型自己的语言码**（hfa：`{"cmn":"zh","eng":"en","jpn":"ja"}`） |
| `defaultLanguage` | ➖ | 调用方未指定语言时采用的语言（A27 后移至 `exports`） |
| `defaultNonSpeechPhonemes` | ➖ | 调用方未指定时检测的非语音音素（默认常量 `{"AP"}`，因为 release 说明将 EP 标为「not recommend」；A27 后移至 `exports`） |

词典文件**不另设键**：`vocab.json` 的 `dictionaries` 是模型自身的约定（模型目录内的相对文件名，Python 参考
同样如此），按 `vocab.json` 所在目录解析。lint 增加一条规则：解析这些文件名并逐个核对文件存在。

### 4.2 加载期互检（不一致即拒绝载入，写法与 game 解释器的 `createExports` 相同）

1. `exports.sampleRate` ≠ `config.json` 的 `mel_spec_config.sample_rate` → 拒绝（错误的采样率会导致静默错位，lite 已实测过同类事故）。
2. `exports.channelCount` ≠ 1 → 拒绝。
3. `exports.languages` 中的每个 id：`configuration.languages` 无映射 → 拒绝；映射出的模型码在 `vocab.json` 的
   `dictionaries` 中没有词条，或对应文件不存在 → 拒绝。
4. `exports.nonSpeechPhonemes` 必须 ⊆ `vocab.json` 的 `non_lexical_phonemes`，否则拒绝。
5. `exports.silenceLabel` 必须属于 `vocab.json` 的 `silent_phonemes`（hfa：`SP`），否则拒绝。
6. `exports.maxSegmentDuration`（60）与模型限制一致；`0` 或缺失不禁止（错误留到运行期报告）。

### 4.3 单次执行的流程（`HfaExecutive::run`）

```
1  prepareSamples(audio, schema)             ← 库函数：采样率/声道/整帧/段上限校验
2  lyrics 非空校验；language → 模型码；nonSpeechPhonemes 取默认值或给定值
3  旋钮经 chooseKnob 取值（越界拒绝），秒 → 帧（frameInterval = hop_size / sample_rate）
4  G2P：lyrics → ph_seq / word_seq / ph_idx_to_word_idx（词不在词典中即跳过，与参考实现相同）
   非静音音素加「模型码/」前缀（silent_phonemes 不加），与参考实现相同
5  session.run({waveform}) → ph_frame_logits / ph_edge_logits / cvnt_logits
6  Viterbi + 边界小数修正 → 音素区间 → 按词聚合（跳过 SP，与参考实现相同）
7  NonLexicalDecoder：cvnt → AP/EP 区间 → 插入（保留 add_AP 的区间扣减与最短时长逻辑）
8  fill_small_gaps（默认 0.1s）→ add_SP（整段）→ clear_language_prefix
9  转换为 AlignResult：绝对秒 = audio.startTime + 局部秒；词/音素两级
10 取消检查位于步骤 4–9 的各步边界；会话 stop()/waitForFinished() 的覆写方式与 rmvpe/game 相同
```

**与参考实现的有意差异**：不做音频解码与重采样（由宿主负责）；不对超过 60 s 的音频自动分段（拒绝，由宿主
切片）；不做 Python 版的 pad 多次推理平均；时间由 float 升为 double 并锚定绝对秒；`SP` 词的输出方式按 D4。

---

## 5. 包与发布

### 5.1 包形状（`packages/hfa` 入库声明 + 打包时装配）

> 下为方案制定时的形状。现行包为 `otter-hfa` 0.2.0.0，声明位于 `inferences/align/inference.json`，
> 接口名为 `org.openvpi.otter.inference.Align`，`exports` 的形状按 A27 修改。

```
otter-hfa/
  desc.json                      id=otter/hfa, version=0.1.0.0, compatVersion=0.1.0.0
  analyzers/align/analysis.json  声明（见下）
  model.onnx                     ← HubertFA v0.0.7（415,220,874 字节）
  config.json                    ← 模型自带
  vocab.json                     ← 模型自带
  ds-zh-pinyin-lite.txt / ds_cmudict-07b.txt / japanese_dict_full.txt   ← 模型自带词典
```

> 包内**不附 `LICENSE`**：HubertFA 仓库采用 Apache-2.0，但其发布 zip 不含许可文件，权重本身也没有
> 单独的许可声明；来源与这一状态写在 release notes 中（与 RMVPE 包的处理一致）。实测见 §10.6。

`analysis.json`（草案）

```json
{
  "interface": "org.openvpi.otter.analysis.Align",
  "level": 1,
  "variant": "hfa",
  "name": "HuBERT-FA",
  "exports": {
    "sampleRate": 44100, "channelCount": 1, "maxSegmentDuration": 60,
    "languages": ["cmn", "eng", "jpn"],
    "nonSpeechPhonemes": ["AP", "EP"],
    "silenceLabel": "SP",
    "knobs": {
      "nonSpeechThreshold":   { "minimum": 0.0, "maximum": 1.0, "default": 0.5 },
      "nonSpeechMinDuration": { "minimum": 0.0, "maximum": 2.0, "default": 0.1 },
      "gapFill":              { "minimum": 0.0, "maximum": 1.0, "default": 0.1 }
    }
  },
  "configuration": {
    "model": "../../model.onnx", "config": "../../config.json", "vocab": "../../vocab.json",
    "languages": { "cmn": "zh", "eng": "en", "jpn": "ja" },
    "defaultLanguage": "cmn",
    "defaultNonSpeechPhonemes": ["AP"]
  }
}
```

> 语言表、词典文件名与 `VERSION` 内容以 **v0.0.7 zip 的实际内容**为准（P0 步骤）。release 说明只写了
> 「Mandarin / Japanese Romanization / English」，是否包含 `yue` 必须解包核对，不能依据说明编写声明。

### 5.2 发布产物与形状（D2）

- `otter-hfa-0.1.0.0.zip`（内含 `otter-hfa/` 包根目录，解压后即为包目录）
- `manifest.json`：`bundleVersion` + 每个包的 `id/file/sha512/version/compatVersion/directory/size/models{}`
  （`models` 为包内每个模型/词典文件的 SHA512，字段与键名与已发布的 manifest 一致）
- GitHub release：tag `models-v0.2.0.0`，标题与正文包括：两个既有包（rmvpe/game）原样搬运与新包 hfa 的说明、
  HFA 权重来源（HubertFA `v0.0.7` `1218_hfa_model_new_dict.zip`，Apache-2.0）与校验和清单。
- 打包步骤脚本化（`scripts/make-package.py`）：装配目录 → `check-declarations.py` → zip（确定性：成员按
  字典序排列、时间戳固定）→ SHA512 → `manifest.json`。**脚本不联网、不发布**；发布使用
  `gh release create/upload`，由用户下令执行。

当前发布状态见文首：三个包的 0.2.0.0 版由 `models-v0.3.0.0` 提供。

---

## 6. 验证方案（门禁，实施后逐条报告数据）

| 层 | 手段 | 通过标准 |
| :-- | :-- | :-- |
| 契约读取 | `test_ManifestValues` 中同类新用例：`readAlignSchema` 正反例 | 反例必须报错（缺 sampleRate、未知键、旋钮域倒置等） |
| 装载互检 | 新 package fixture（`src/tests/auto/Analysis/packages/*`）＋ `test_AnalysisLoad` | §4.2 六条各有一例被拒绝 |
| 执行面 | `test_Hfa`：`make-model-fixtures.py` 新增 hfa 图（真实签名、假权重，输出可预测） | 词/音素时间位置、旋钮确实传入图、取消返回 Cancelled、绝对秒锚定（`startTime=30`） |
| 真实权重 | 用 v0.0.7 模型 + 真实演唱片段运行一次，检查区间单调、覆盖整段、AP 段位置 | 无异常、区间自洽 |
| **等价性** | 同模型、同音频、同歌词：otter 解释器与 `dstools-cli hfa` 的 `{phone,start,end}` 逐条比对 | 音素数一致；时间差 ≤ 1 帧（10 ms）并列出最大差；出现系统性偏离时逐条定位到算法步骤 |
| 打包 | `check-declarations.py`；zip 解压树清单 + 每个文件的 SHA512 | 0 error；`manifest.json` 中的 SHA512 与实测一致 |
| 回归 | `ctest`（含既有 7 个用例）＋ `python -m unittest discover -s scripts` | 无新增失败 |

> 等价性比对是本方案的核心门禁：参考实现与移植版是**同一套算法**，预期结果不是近似，而是逐音素接近
> 逐位一致；出现差异即为移植错误，不接受以调参掩盖。比对前先以同一输入运行 oracle 两次，确认其输出稳定。

---

## 7. 实施分期

| 期 | 内容 | 交付物 |
| :-- | :-- | :-- |
| **P0** | 下载 v0.0.7 zip 并解包核对（`config.json`/`vocab.json`/`VERSION`/词典/许可）；确定语言表与文件清单 | 核对记录（§10.1） |
| **P1** | `Align` 契约：头文件 + 读取函数 + CMake + JSON Schema + lint 扩展与自测 + 文档（README 表格、`otter-design.md` 台账 A19 起） | `include/otter/Api/Align/1/AlignApiL1.h`、`src/lib/Api/Align/1/AlignApiL1.cpp`、`docs/schemas/align-1-exports.schema.json`、lint 修改 |
| **P2** | `hfa` 解释器：算法移植 + 外壳 + 构建接线 | `hfa/{main.cpp,plugin.json,CMakeLists.txt}` 及插件目录的 `CMakeLists.txt`（现位于 `src/plugins/inferenceinterpreters/`） |
| **P3** | 测试与 fixture：`test_Hfa` + hfa fixture 图 + 装载互检 fixture | `src/tests/auto/Analysis/test_Hfa.cpp`、`scripts/make-model-fixtures.py` 新增部分、`src/tests/auto/Analysis/CMakeLists.txt` |
| **P4** | 包声明与装配：`packages/hfa`、`scripts/make-package.py`、`packages/README.md` | 声明 + 装配脚本 |
| **P5** | 真实权重验证 + 等价比对 + 打包 + **发布（由用户下令）** | 验证报告、`otter-hfa-0.1.0.0.zip`、`manifest.json`、release |

**构建约束**：每期结束时在本机既有构建树（Ninja + Release + `OTTER_BUILD_TESTS=ON`，使用 lite 的 vcpkg）中
重新构建并运行 `ctest`；不为 otter 引入新依赖。

---

## 8. 风险与未决

1. **词典与 `model.onnx` 的许可**（**已结案**，P0/P5）：HubertFA 仓库采用 Apache-2.0，其发布 zip **不含**
   `LICENSE`，权重本身也没有单独声明；release notes 如实写明「权重随 HubertFA v0.0.7 发布，无单独许可声明」，
   包内不附许可文件（与 RMVPE 包的处理一致）。实测见 §10.1 与 §10.6。
2. **`merged_phoneme_groups` 未读取**：Python 参考用它合并音素组（折叠 `EP/SP/CL/…` 等静音族），C++ 参考从未
   读取。本轮遵循 C++ 参考；若日后发现与 Python 输出存在系统性差异，另立条目。
3. **歌词形态由宿主保证**：契约只接收字符串，词表的正确性（拼音/罗马字与词典一致）由调用方负责；词典中没有
   的词被静默跳过（与参考实现相同）。是否在结果中报告被跳过的词留待 L2。
4. **`Align` 是否需要逐词置信度**：HFA 有逐帧置信度，本轮不进契约，待 lite 的需求确定后再议。
5. **跨段边界**：与 Note 相同，模型没有跨段连续性的入口；宿主的切片决策仍然适用（`maxSegmentDuration=60`）。
6. **等价性比对的语言覆盖**：模型自带 zh/ja/en 三本词典（`vocab.json` 的 `dictionaries` 只有这三本，`yue`
   没有词典），比对只覆盖现有素材的语言（cmn）；其余语言不做等价性声明。

---

## 9. 决策台账（本方案编号，已并入 `docs/otter-design.md` 的 A 系列）

| 编号 | 决策 | 依据 / 来源 | 状态 |
| :-- | :-- | :-- | :-- |
| D1 | 新立 `Align` L1 契约，与 F0、Note 同属一个类别 | 规范 2.4「输入输出根本不同时另立 interface」；`otter-design.md` A4 | **已实施**（P1）；并入 A19。类别由 otter 自有的 `analysis` 改为内置 `inference`（A26） |
| D2 | 发布形态：新 tag + 三包 + 全量 manifest | `models-v0.1.0.0` 的已发布形状 | **已实施**（P5：`models-v0.2.0.0`）；并入 A25。现行发布为 `models-v0.3.0.0` |
| D3 | 授权：下载模型 / 本地构建 / 创建 release | 联网、构建、远程写操作各需用户下令 | **已实施**（逐项下令） |
| D4 | `lyrics` 为单个字符串；结果覆盖整段；静音以 `silenceLabel` 出现 | 参考实现 `Hfa.cpp:132-208`、`AlignWord.cpp:278-321` | **已实施**（P1/P2）；并入 A20、A22 |
| D5 | 三个旋钮（非语音阈值/最短时长/补缝）+ `maxSegmentDuration=60` | `NonLexicalDecoder.cpp:83`、`AlignWord.h:157,161`、`Hfa.cpp:161-164` | **已实施**（P1/P2）；并入 A23、A24 |
| D6 | lite 接入不在本轮 | 范围控制（需求只包括 otter 与发布） | **已确认**（本轮未实施） |
| D7 | 词典路径由 `vocab.json` 的 `dictionaries`（模型自身的约定）给出，lint 核对文件存在 | Python 参考 `infer_base.py`（`vocab_folder / vocab["dictionaries"]`） | **已实施**（P4），并据此发现漏装，见 §10.5；并入 A21 |
| D8 | 语言 id 使用宿主词汇，模型码写在 `configuration.languages` | [`lite-integration.md`](../lite-integration.md) §6「语言标识采用宿主的词汇表」 | **已实施**（P4）；A27 进一步规定 Align 的语言项形状 |
| D9 | 打包步骤脚本化（装配/校验/zip/SHA512/manifest），发布动作由用户下令 | 可复现性；部署与发布由用户下令 | **已实施**（P4 脚本；P5 先报告校验和，用户下令后创建 release） |

全部推荐项均经用户确认：D1–D3 逐条选定，D4–D6 为范围确认，D7–D9 为实施方式确认。

---

## 10. 实施记录

### 10.1 P0 解包核对（模型 zip）

| 项 | 实测值 |
| :-- | :-- |
| 资产 | `1218_hfa_model_new_dict.zip`（HubertFA v0.0.7 release），256,589,553 字节 |
| SHA512 | `B3AC897F913FF3529704A8169E5C54F658C0658E5201611C7B4069B0F750C1DAA9ED576D3444B989670EDC78DE3D359644088966865371907D1408A40749971E` |
| 解包内容 | `model.onnx`（415,220,874 字节）、`config.json`、`vocab.json`、`VERSION`（内容为 `5`）、三本词典；**无 `LICENSE`** |
| `config.json` | `mel_spec_config.sample_rate = 44100`、`hop_size = 441`（10 ms 帧） |
| `vocab.json` | `dictionaries = {en: ds_cmudict-07b.txt, ja: japanese_dict_full.txt, zh: ds-zh-pinyin-lite.txt}`；`non_lexical_phonemes = [AP, EP]`；`silent_phonemes` 9 项（含空串与 `SP`）；`language_prefix = true`；`merged_phoneme_groups` 存在但 C++ 参考不读取 |
| 词典 | zh `ds-zh-pinyin-lite.txt` 615 行 / 6,494 字节（键为拼音音节，如 `bai → b ai`）；en `ds_cmudict-07b.txt` 133,805 行 / 3,402,616 字节（键为英文词，音素为 ARPAbet，如 `aa l ow`）；ja `japanese_dict_full.txt` 179 行 / 1,564 字节（键为罗马字音节，如 `ka → k a`，另含 `SP`/`AP` 两条）；三份均为 CRLF，无空行，无缺少 Tab 的行 |
| 结论 | 包内不附许可文件，release notes 如实写明权重来源与许可状态（与 RMVPE 包一致）；`lyrics` 的形式随语言变化（cmn 为拼音音节，eng 为单词，jpn 为罗马字音节），因此契约只接收字符串 |

### 10.2 P1 契约（`Align` Level 1）验收

| 门禁 | 结果 |
| :-- | :-- |
| 头文件自洽 | `g++ -std=c++17 -fsyntax-only` 通过；MSVC 构建通过 |
| 库构建 | `otter` 目标 11/11 编译并链接（新的 `AlignApiL1.cpp` 由 GLOB 收入） |
| lint 自测 | `python -m unittest discover -s scripts -p "test_*.py"` → Ran 11 tests … OK |
| 发布面 | `docs/schemas/align-1-exports.schema.json`、`align-1-import-options.schema.json` 已提交并通过 JSON 校验 |
| 文档同步 | `README.md` 契约表由两份改为三份；`otter-design.md` §1 表、新增 §5.4（Align 契约面）、§6 包形状（新增 hfa 两段）、§7 插件名、台账 **A19–A25**、§10 里程碑 M7、§11 未决 |

### 10.3 P2 解释器验收

| 门禁 | 结果 |
| :-- | :-- |
| 构建 | `otterhfa` 目标 5/5 编译并链接（Ninja + MSVC 14.51 + vcpkg 工具链） |
| 移植自检 | `Aligner.cpp` / `G2p.cpp` / `NonSpeech.cpp` / `main.cpp` 的 `g++ -std=c++17 -fsyntax-only` 均通过 |
| 逐行复核 | 按 `AlignmentDecoder.cpp:150-544`、`AlignWord.{h,cpp}`、`DictionaryG2P.cpp:59-110`、`NonLexicalDecoder.cpp` 逐函数对照复核，发现并修复两处保真缺口：词内续接的 1e-6 容差、`fill_small_gaps` 的负起点钳位 |
| 外部 oracle 对拍 | 非语音解码：200 次 decode / 757 个区间，`start` 与 `duration` **逐位一致**；G2P：10 行歌词的 `phonemes / words / phonemeToWord` 逐字一致。对拍中修复一处缺陷：区间终点必须按 `float(end) * float(frameInterval)` 计算，且减法在 double 中进行（原写法在 757 个区间中有 108 个相差 37–90 ns） |

**相对参考实现的有意差异（均已写入注释）**：

| 项 | 参考实现 | 本移植 | 理由 |
| :-- | :-- | :-- | :-- |
| 越界/空数据 | 记录日志后继续，或直接越界读取 | 返回 `srt::Error` | 契约要求失败必须报告，静默继续会把错误答案当作结果 |
| 词典中无 Tab 的行 | 警告并跳过 | `InvalidFormat` 拒绝载入 | 该行与空词条无法区分，属于数据损坏 |
| 帧数 > logits 实际帧数 | 越界读取 | `InvalidArgument` | 同上；已发布的图不会出现此情形 |
| `ph_frame_pred_` / `total_confidence_` | 计算并保留为成员 | 丢弃 | 下游无读取者；帧置信度按等价性比对的需要保留在解码内部 |
| `scoreScale`（T÷S） | 在循环内每次重新计算 | 提到循环外 | 操作数相同，结果逐位一致 |
| 退出码/异常 | `try/catch` + `msg` 字符串 | `srt::Expected` | 契约面要求 |

### 10.4 P2 修正：Viterbi 通路的缺陷（fixture 实测发现）

P2 的验收只包括构建与逐行复核，没有实际运行解码通路。P3 的 fixture 接通后，首次运行即暴露两处缺陷：

| 项 | 症状 | 根因 | 修正 |
| :-- | :-- | :-- | :-- |
| **解码只剩一个类**（主缺陷） | fixture 下 `decode` 返回 0 个词（`ignoreSp=true`），或整段为 1 个覆盖 [0, 1.0] 的 SP 词；帧级 Viterbi 全程没有发生转移 | `Aligner.cpp` 中 `std::vector<std::vector<float>> frameLogits{phFrameLogits[0]};`：本移植的接口接收的已是批次 0 的 [类][帧] 矩阵，此处又取了第 0 行，只剩 SP 一行；参考实现的 `decode` 接收三维数据并自行切出 `[0]`，两者接口不同 | `std::vector<std::vector<float>> frameLogits = phFrameLogits;`（复制整个矩阵，供掩码就地修改） |
| **未知歌词静默变为静音** | 歌词中没有任何词在词典中时（如 `"zzz"`）不报错，输出一段纯静音的对齐结果 | `G2p::split` 对未知词只插入 SP 分隔符，`phrase.phonemes` 非空，原判断 `phrase.phonemes.empty()` 无法拦截 | 改为判断 `phrase.words.empty()`，报错文本仍为「none of the lyrics is in this language's dictionary」 |

取 `[0]` 的那一行旁原有一条注释，将其解释为绕开 mingw g++ 16.1 拒绝从元素复制初始化 `vector<vector<float>>`
的写法。该编译错误实际表明 `phFrameLogits[0]` 的类型是 `vector<float>`，即接口本应接收二维数据；注释对错误的
解释是错误的，逐行复核也因此将这一行视为工具链适配而放过。教训：**注释给出的动机不能代替对接口签名本身的
核对**（参考实现为三维 `decode`，本移植为二维 `decode`，对照签名即可发现）。

**修正验证**（临时探针，未入库）：以 fixture 的时序直接调用 `Aligner::decode`，修正前为 0 个词，修正后为
`a=[0.195, 0.395]`、`b=[0.595, 0.795]`、AP 区间 `[0.43, 0.54]`，与设计帧（20/40/60/80、呼吸 43–54）的偏差
恰为边界小数修正允许的 ±0.5 帧。修正后 `test_Hfa` 的 5 个用例与既有 7 个用例在 ctest 下全部通过（8/8）。

该缺陷表明，移植的**逐行复核与数值等价是两件事，前者不能证明后者**；因此 P5 的真实权重对拍不可省略。

### 10.5 P4 打包脚本发现的漏装

`make-package.py` 首次运行即报告 3 个错误：三本词典未装入包。原因是**声明没有列出全部文件**：
`analysis.json` 只列出 `model.onnx` / `config.json` / `vocab.json`，词典由模型自带的 `vocab.json` 的
`dictionaries` 列出，解释器读取的也是后者。脚本据此增加规则：模型自带 JSON 所列出的伴随文件同样必须装入，
该规则已写入脚本的文档字符串。手工装配时的同类漏装要到宿主加载时才会暴露，脚本内置的 lint 将其提前到打包期。

**确定性**：同一输入装配并打包两次（第二次输出到另一目录），`otter-hfa-0.1.0.0.zip` 的 SHA512 一致：
`27d4d6dc7a705fd73d72dec0531fe8abdd489ab14caf022d36b0de06a236b6e11b233ace86cbbed9f1006554b77fb381a83d51afe085f5280d67b2a4c98ff076`（265,940,592 字节）。

### 10.6 P5 真实权重验收（v0.0.7 `1218_hfa_model_new_dict.zip`）

| 项 | 结果 |
| :-- | :-- |
| 权重 | 从 256,589,553 字节的官方 zip 解出 `model.onnx`（415,220,874 字节）+ `config.json` + `vocab.json` + 三本词典 |
| 包装配 | `scripts/make-package.py --variant hfa`：lint **0 error**；`otter-hfa-0.1.0.0.zip` 包含 `desc.json`、`analyzers/align/analysis.json` 与 6 个模型文件（无 LICENSE，理由见下），SHA512 见 §10.5 |
| 许可 | HubertFA 仓库采用 Apache-2.0，但其发布 zip 不含许可文件，权重本身未单独声明许可；包内不附 LICENSE，在 release notes 中写明来源与这一状态（与 RMVPE 包一致） |
| 真实推理 | 以 `BaiMaRuLuHua_0.wav`（44.1 kHz 单声道，5.6936 s）及其 `.lab` 歌词为输入，经**打包产出的包**、真实解释器与 CPU 驱动运行：15 个词 / 27 个音素区间 |
| 与参考二进制对拍 | `dstools-cli hfa --model … --wav … --lab …`（dataset-tools `cmake-build-debug` 构建）产出 27 个音素区间；逐区间比对：**标签序列完全一致，最大边界差 5.74e-7 s（起点）/ 7.54e-7 s（终点）**，属于 float32 舍入量级，比一帧（0.01 s）小四个数量级 |
| 旧包复用 | 重新下载 `otter-rmvpe-0.1.0.0.zip` / `otter-game-0.1.0.0.zip` 并逐字节核对：大小与 SHA512 均与已发布的 `manifest.json` 一致（334,205,733 / 45,713,320 字节） |

对拍使用的探针与比对脚本位于临时目录，未入库。

该结果是 P2 所需的等价性证据：修正 §10.4 的缺陷后，本移植与参考实现在同一权重、同一音频上给出同一组边界，
误差仅在浮点末位。

### 10.7 P5 发布（`models-v0.2.0.0`，历史版本）

| 项 | 值 |
| :-- | :-- |
| tag / URL | `models-v0.2.0.0` — <https://github.com/diffscope/otter/releases/tag/models-v0.2.0.0>（非 draft、非 prerelease；`models-v0.1.0.0` 保留） |
| 资产（上传后回读的大小） | `otter-hfa-0.1.0.0.zip` 265,940,592；`otter-rmvpe-0.1.0.0.zip` 334,205,733；`otter-game-0.1.0.0.zip` 45,713,320；`manifest.json` 3,398（`bundleVersion 0.2.0.0`，三个包） |
| 上传后核对 | 四个资产的大小与本地一致；回下载的 `manifest.json` 与本地逐字节一致，三个包的 SHA512 与 manifest 条目一致 |
| 发布说明 | 权重来源与许可状态、与参考实现的等价性实测、fixture 测试、可复现性、旧包复用前的核对 |
| 授权 | 先报告校验和与资产清单，用户下令后执行 `gh release create` |

`models-v0.2.0.0` 装的是旧布局的 0.1.0.0 包，当前 otter 不能载入。A26 之后三个包升级为 0.2.0.0，由
`models-v0.3.0.0` 提供。

**维护说明**：
1. `packages/hfa/` 的声明与 `scripts/make-package.py` 是包的可复现来源，修改版本号后重新运行即可（相同输入的
   产物逐字节一致，见 §10.5）。
2. 权重不入库，重新发布 hfa 包需要 HubertFA v0.0.7 的 `1218_hfa_model_new_dict.zip`（SHA512 见 §10.1）。
3. 契约面修改时，需同步 `docs/schemas/align-1-*.json` 与 `src/tests/auto/Analysis/test_Hfa.cpp`。
4. 三个包都只声明宿主需要的事实；在装配后的目录上运行 `scripts/check-declarations.py` 是发布前的硬门禁。
5. 移动已发布 release 的 tag（删除并重建同名 ref）会使 GitHub 将该 release 置为 draft，对外不可见；重建 tag
   不会恢复公开状态，必须另行执行 `gh release edit --draft=false`。`models-v0.2.0.0` 的 tag 曾因此移动到包含
   hfa 源码的提交，并已恢复为公开状态。

### 10.8 收尾

- 实施后的只读审计发现 24 处文档与代码状态不一致，已逐条修正：三个变体/三个解释器的表述、Align 不再写作
  「将来」、包根目录名与 zip 名（`otter-hfa`）、rmvpe 声明示例、GAME 声明块与随包发布的声明逐字段一致、
  M7 标为完成、RMVPE 的验证状态（权重已发布、数值未比对）、ctest 用例数、文档间的交叉引用、lite-integration.md
  的范围说明（不含 Align）。`otter-design.md` §9.5 联合审计与 A17 依据中的「两个提供者/两个变体」保留，因为它们记录的是当时的事实。
- 实施提交已整理为三个提交（契约 / 解释器与 fixture / 包与脚本），并推送到 `analysis-level-1`。
