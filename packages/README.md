# 已发布变体的包声明

本目录保存**三个出厂变体**的包声明。模型权重不纳入版本管理（见 `.gitignore`）。

| 目录 | 契约 | 变体 | 模型（打包时复制） |
| :-- | :-- | :-- | :-- |
| `rmvpe/` | `org.openvpi.otter.inference.F0` | `rmvpe` | `rmvpe.onnx`（16 kHz，345 MB） |
| `game/` | `org.openvpi.otter.inference.Note` | `game` | `encoder`、`segmenter`、`estimator`、`bd2dur`、`dur2bd` 五个 ONNX 模型（44.1 kHz） |
| `hfa/` | `org.openvpi.otter.inference.Align` | `hfa` | `model.onnx` 及模型自带的 `config.json`、`vocab.json` 与三部词典（44.1 kHz） |

三个声明的版本均为 0.2.0.0，采用 `inferences/` 布局（A26）。release `models-v0.3.0.0` 提供由这三个
声明装配的包 `otter-rmvpe`、`otter-game`、`otter-hfa`，附带 `manifest.json`（`bundleVersion` 为
0.3.0.0）。此前的 release `models-v0.1.0.0` 与 `models-v0.2.0.0` 包含 0.1.0.0 版的旧布局包，当前
otter 不再能载入这些包。

## 包的装配

包目录即 `desc.json` 所在的目录。`scripts/make-package.py` 按声明把模型复制到位、运行校验、
打成 zip 并计算校验和：

```sh
python3 scripts/make-package.py --variant hfa \
    --models /path/to/1218_hfa_model_new_dict \
    --manifest build/packages/manifest.json --bundle 0.3.0.0
```

`--models` 指向模型文件所在的目录。脚本先按文件在包内的路径查找，未找到时再按**文件名**查找；
若包内两个文件同名，脚本拒绝按文件名查找，此时须按包的布局组织 `--models` 目录。相同输入的
两次打包结果逐字节一致。产物写入 `--output`（默认 `build/packages`）：装配好的包目录、
`otter-hfa-0.2.0.0.zip`，以及输出到标准输出的 manifest 片段；指定 `--manifest` 时，片段同时并入
该文件。

装配的实质操作是复制声明并放入模型，手工装配与之等价：

```sh
cp -r packages/hfa /path/to/otter-hfa
cp /path/to/1218_hfa_model_new_dict/{model.onnx,config.json,vocab.json} /path/to/otter-hfa/
cp /path/to/1218_hfa_model_new_dict/{ds-zh-pinyin-lite,ds_cmudict-07b,japanese_dict_full}.txt /path/to/otter-hfa/
```

## 发布前校验

`make-package.py` 会自动运行以下校验；手工装配时不得省略：

```sh
python3 scripts/check-declarations.py /path/to/otter-hfa /path/to/otter-rmvpe /path/to/otter-game
```

仓库中的声明不含模型文件，CI 以 `--declarations-only` 模式只检查声明本身，不打开声明引用的
任何文件：

```sh
python3 scripts/check-declarations.py --declarations-only packages/rmvpe packages/game packages/hfa
```

`errors` 必须为 0。完整模式检查以下各项：声明引用的文件均存在，`configuration` 中的键均为变体
读取的键，`exports` 与 `configuration` 相互一致；这些是本目录中最易出错之处。对 `hfa` 另有
一组检查，所读取的均为模型自带的文件：`exports` 中的采样率须与模型 `config.json` 的
`mel_spec_config.sample_rate` 一致，声明的每种语言须有词典，非语音标签须在词表中，静音标签须在
`silent_phonemes` 中。

## 相对路径的基准

规范 2.4 规定，相对路径以该字段所在声明文件的目录为基准。模型与声明文件不在同一目录，因此
`inferences/*/inference.json` 中的路径写作 `../../<model>.onnx`。若写作 `./<model>.onnx`，路径将
指向声明文件自身的目录，校验器报告 `refers to a missing file`。
