# 已发布变体的包声明

本目录保存**两个出厂变体**的包声明，模型权重不入库（见 `.gitignore`）：

| 目录 | 契约 | 变体 | 模型（打包时复制进来） |
| :-- | :-- | :-- | :-- |
| `rmvpe/` | `org.openvpi.otter.analysis.F0` | `rmvpe` | `rmvpe.onnx`（16 kHz，345 MB） |
| `game/` | `org.openvpi.otter.analysis.Note` | `game` | `encoder` `segmenter` `estimator` `bd2dur` `dur2bd` 五个 ONNX（44.1 kHz） |

## 做成可安装的包

包目录就是 `desc.json` 所在的那一层。装配一个包 = 复制声明 + 放进模型：

```sh
cp -r packages/rmvpe /path/to/otter-rmvpe@0.1.0.0
cp /path/to/rmvpe.onnx /path/to/otter-rmvpe@0.1.0.0/rmvpe.onnx
cp -r packages/game /path/to/otter-game@0.1.0.0
cp /path/to/GAME-1.0.3-small-onnx/{encoder,segmenter,estimator,bd2dur,dur2bd}.onnx \
   /path/to/otter-game@0.1.0.0/
```

## 发布前必须过校验

```sh
python3 scripts/check-declarations.py /path/to/otter-rmvpe@0.1.0.0 /path/to/otter-game@0.1.0.0
```

`errors` 必须为 0。这条门禁查的就是"声明指的文件确实在、键是变体读的那些、`exports` 与
`configuration` 自洽"，全部是本目录最容易写错的地方。

## 相对路径的基准是**声明文件自己所在的目录**

规范 2.4 规定相对路径以该字段所在声明文件为基准。模型与清单不在一层，所以
`analyzers/*/analysis.json` 里写的是 `../../<model>.onnx`——写成 `./<model>.onnx` 会指向
清单自己的目录，校验器会报 `points at a file that is not there`。
