# The ggml providers

> 中文：[ggml-providers.zh.md](ggml-providers.zh.md) — the Chinese counterpart of this document.

The shipped variants run ONNX models through dsinfer. Two more variants exist beside them: `game-ggml` and `tifa-ggml` drive the ggml engines of [game.cpp](https://github.com/KakaruHayate/game.cpp) and [tifa.cpp](https://github.com/KakaruHayate/tifa.cpp) **as external processes**. They serve the same Level 1 contracts the ONNX variants serve — `org.openvpi.otter.inference.Note` and `org.openvpi.otter.inference.Align` — so a host offers both under one settings page, and a package picks its backend by naming one variant or the other in its manifest.

The provider never touches the engine's weights or the ggml backends: the package names the engine's command line tool, and the provider launches it, hands it one span and reads the answer back. The game engine runs as one long-lived `serve` process per analyzer (a binary request frame on its standard input, one JSON line of notes on its standard output); the tifa engine runs one `align` command per execution and answers with a TextGrid. Everything the engine needs — its executable, the ggml DLLs of its release, the weights, the dictionaries — stays outside the host process, exactly where the engine's own release puts them.

## What "external" means here

These two variants are a different kind of package, and the difference is worth stating before anything else:

+ **The engine is neither part of this repository nor of the model release.** Release `models-v0.1` holds the four ONNX packages only (`otter-game`, `otter-hfa`, `otter-rmvpe`, `otter-tifa`). To offer a ggml variant, a host downloads an engine archive from the engine's own release.
+ **A package is an engine archive plus a declaration.** It must hold the engine's executable, the GGUF weights and the ggml runtime libraries of that release (and, for tifa, the G2P dictionary tree). The declaration names them through the `cli`, `model` and `dictionaries` keys of its `configuration`.
+ **There is no fallback.** An engine archive that is missing, or a `cli` path that does not resolve, fails analyzer creation with `cannot find the game CLI at <path>` or `cannot find the tifa CLI at <path>` — the host is never quietly handed the ONNX variant instead. A host therefore offers these variants only where an engine is installed.
+ **The engine archive is itself an OpenUtau dependency package:** it carries an `oudep.yaml` whose `entrypoints` is `loader: Executable` pointing at the engine's executable.

## Installing an engine

The engines ship as release archives the host installs the way it installs the model packages of [docs/packages.md](packages.md): unpack the archive anywhere and name the unpacked files from the declaration.

+ **game.cpp**: unpack `game_ggml-<platform>-<backend>[-q8].oudep` (a zip) from the [v0.1.3 release](https://github.com/KakaruHayate/game.cpp/releases/tag/v0.1.3), which holds `game_ggml_cli.exe`, `game_medium.gguf`, the ggml DLLs, `config.json` and `oudep.yaml`.
+ **tifa.cpp**: unpack `tifa-cli-<platform>-<precision>.tar.gz` from the [v0.1.6 release](https://github.com/KakaruHayate/tifa.cpp/releases/tag/v0.1.6) (a CUDA build is named `tifa-cuda-<platform>-<precision>.tar.gz`; the q4 builds are `tifa-ggml-<platform>-q4.oudep` in the separate tag [`oudep`](https://github.com/KakaruHayate/tifa.cpp/releases/tag/oudep)), which holds `tifa_ggml_cli.exe`, the `models/` directory with the GGUF weights, the G2P dictionaries and `cpp_pinyin/`.

The declarations under `docs/examples/packages` were written and measured against these two releases: game.cpp **v0.1.3** (`game_ggml-windows-x64-vulkan.oudep`, the non-quantized build, [download](https://github.com/KakaruHayate/game.cpp/releases/download/v0.1.3/game_ggml-windows-x64-vulkan.oudep)) and tifa.cpp **v0.1.6** (`tifa-cli-windows-x64-full.tar.gz`, [download](https://github.com/KakaruHayate/tifa.cpp/releases/download/v0.1.6/tifa-cli-windows-x64-full.tar.gz)). A newer engine release is likely to work, but its `config.json` and its `--help` are the authority on the keys above. The examples and the file lists of this document name the Windows binaries, because that is what they were measured on; another platform's archive carries its own executable and library names, and `cli` has to name that file.

An unpacked engine goes beside the declaration that names it. Both examples read their files two levels up from the declaration, so the archive's contents land next to `desc.json`:

```text
docs/examples/packages/tifa-ggml/            docs/examples/packages/game-ggml/
  desc.json                                    desc.json
  inferences/align/inference.json              inferences/note/inference.json
  tifa_ggml_cli.exe                            game_ggml_cli.exe
  models/tifa.gguf                             game_medium.gguf
  models/dictionaries/, models/cpp_pinyin/     config.json, ggml*.dll
  ggml*.dll, msvcp140*.dll, vcruntime140*.dll
```

| Engine | Archives published | Platforms and backends |
| :-- | :-- | :-- |
| game.cpp | `game_ggml-<platform>-<backend>[-q8].oudep` | `windows-x64`, `linux-x64`, `macos-x64`, `macos-arm64`; backends `vulkan`, `cuda`, `metal`, `cpu` (the Windows releases carry `vulkan` and `cuda`); `-q8` names the quantized build, which is smaller |
| tifa.cpp | `tifa-cli-<platform>-<precision>.tar.gz`, `tifa-cuda-<platform>-<precision>.tar.gz` | `windows-x64`, `linux-x64`, `macos-arm64`; precision `full` or `q4` |

What a runnable install holds beside each other:

| Variant | Files in one directory |
| :-- | :-- |
| `game-ggml` | `game_ggml_cli.exe`, `game_medium.gguf` (about 190 MB), the `ggml*.dll` of the release (the backend DLL included, e.g. `ggml-vulkan.dll`) and `config.json` |
| `tifa-ggml` | `tifa_ggml_cli.exe`, `models/` (`tifa.gguf`, about 80 MB, `dictionaries/`, `cpp_pinyin/`, the `breath-*.gguf` files it needs) and the `ggml*.dll`; on Windows also the MSVC runtime DLLs the release ships (`msvcp140*.dll`, `vcruntime140*.dll`) |

Japanese lyrics written in kanji need one more asset: the engine's own README asks for [`unidic-lite-dicdir.zip`](https://github.com/KakaruHayate/tifa.cpp/releases/download/v0.1.6/unidic-lite-dicdir.zip) of the v0.1.6 release to be unpacked as `models/unidic/`. Kana lyrics work without it.

The configuration block of a declaration names those files, relative to the declaration itself:

| Variant | Contract | `configuration` |
| :-- | :-- | :-- |
| `game-ggml` | `Note` | `cli` (the engine's executable), `model` (the GGUF), `languages` (identifier → model numbering), `timestep` |
| `tifa-ggml` | `Align` | `cli`, `model`, `dictionaries` (the G2P dictionary tree), `languages` (identifier → engine code) |

Two example declarations, written against real releases, live under [docs/examples/packages](examples/packages): `game-ggml` (GAME-1.0-medium) and `tifa-ggml` (TIFA-1.0-ST). They pass the lint as they are under `--declarations-only`, the mode that checks declarations without opening the files they name: the engine files are not part of this repository, so the full mode reports them missing. `make-package.py` assembles them the way it assembles the factory packages — point `--models` at the folder the engine archive unpacks to, the one that holds the engine's executable (the game `.oudep` unpacks flat, the tifa `tar.gz` unpacks into a folder of its own), and the packager copies the engine, the model and the dictionaries into the package. Their `version` and `compatVersion` are pinned to the project version of the tree that carries them, as the lint requires of every declaration, so a project version bump must bump them with it; the lint names the file that lags.

```sh
python3 scripts/make-package.py --variant game-ggml \
    --declarations docs/examples/packages \
    --models /path/to/unpacked/game_ggml-windows-x64-vulkan-q8 \
    --manifest build/packages/manifest.json
```

The engine's own runtime libraries (the `ggml*.dll` files beside the tool on Windows) are what the tool loads at run time, and an archive assembled from the declaration alone does not name them. The same goes for the archive's `config.json` and `oudep.yaml`: the declaration never names them, so only a copy of the unpacked directory carries them. The simplest install is therefore to unpack the release archive first and put the declaration into it: copy `desc.json` and the `inferences/` directory of the assembled package into the unpacked archive, and the whole directory is at once an engine install and an otter package.

The `config.json` of an unpacked game archive is the authority on three of the keys above — it reads `samplerate: 44100`, `timestep: 0.01` and `languages: { en: 1, ja: 2, yue: 3, zh: 4 }` in v0.1.3 — so a declaration whose `sampleRate`, `timestep` or `languages` mapping disagrees with it produces a plausible result at the wrong speed or in the wrong language. The `dictionaries` key of a tifa declaration becomes the `--dict-dir` argument of the tool, and it names the **engine's model directory** — `models/`, the one that holds `dictionaries/`, `cpp_pinyin/` and the loose G2P `.txt` files — because the tool looks for `dictionaries/...` below whatever directory it is handed. Naming `models/dictionaries` instead of `models` is the mistake to avoid: the tool then looks for `models/dictionaries/dictionaries/...` and reports a missing dictionary. Leaving the key out passes no argument and the engine looks in its own default place.

A declaration whose exports disagree with the model still passes the lint, because the lint cannot read a GGUF. What the lint cannot check the host owns: the exports' `sampleRate` must be the rate the model was trained at (44100 for GAME, 48000 for TIFA), or the engine produces a plausible result at the wrong speed. `scripts/read-ggml-metadata.py` prints what a declaration needs from a GGUF — the audio format, the language numbering of a game model, the phonemes of every language a tifa vocabulary carries:

```sh
python3 scripts/read-ggml-metadata.py --variant game <model.gguf>
python3 scripts/read-ggml-metadata.py --variant tifa <model.gguf>
```

## Building

The providers need no dsinfer, no ONNX Runtime and no source of the engines, so they build beside the rest of the plugins by default and add nothing to configure:

```sh
cmake -B build/cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
    ... \
    -DOTTER_BUILD_GAME_GGML=ON -DOTTER_BUILD_TIFA_GGML=ON
```

Both options default to on; turning one off leaves that provider out of the deployment. The only code the two share with each other is `otter_cli_support`, the private static library that spawns the child process, writes the span as a float32 WAV and keeps the scratch directory of one execution.

## What a host can observe

The ggml variants share the contract surface with their ONNX siblings and differ from them in points a host sees:

+ The engine call is monolithic: `game-ggml` reports progress 0 and then 1, `tifa-ggml` reports 0, 0.7 and 1. A stop, however, kills the engine process — the game variant's next execution relaunches it and pays the model load again, and a tifa execution reports the cancellation as soon as its tool is gone.
+ `game-ggml` carries no alignment path, because the serve protocol runs no session for the model that converts known durations into boundaries, so its exports cannot declare `supportsKnownNotes`, and an execution that supplies known notes is refused.
+ `tifa-ggml` aligns the first pronunciation the G2P offers for a polyphonic word, where the ONNX variant scores the readings against the audio and picks one. A caller who needs another reading writes the lyrics as the scheme spelling that dictionary entry carries.
+ `game-ggml` seeds the engine's sampling randomness with a fixed value, so repeated transcriptions of one span in fresh analyzers agree byte for byte. Within one long-lived analyzer the first execution of a span was measured to differ from the later ones (23 notes against 22 on one take) — a host that wants the same answer every time builds a new analyzer, or must not assume the first execution equals the rest. Whether the engine treats seed zero as a request for a random one, as an earlier note in this document claimed, was not reproduced against the v0.1.3 Vulkan build: seed 0 and seed 1 each reproduced their own reading exactly, and they differed only in the last digits of the pitch sum.
+ The engine's own progress notes go to the host's standard error, which a GUI host may want to capture.
+ Its `maxSegmentDuration` is not its ONNX sibling's: `game-ggml` declares 30 s where `game` declares 60 s, and the engine refuses a longer span outright (`this model accepts at most 30.000000 seconds in one execution`), so a host that slices at 60 s must slice at 30 s for this variant.

## What a run costs

+ A `tifa-ggml` execution spawns one process and loads the model again every time: measured at about 0.55 s of fixed cost (process start and model load) plus roughly 0.11 s per second of audio — a 7.4 s span took 1.4 s and a 39 s span 5.0 s, the tool's own inference included (Windows x64, the v0.1.6 build). A host that calls align once per bar pays the fixed cost repeatedly; a long-lived `serve` mode would remove it.
+ The engine I/O has **no timeout**: an engine that hangs leaves the execution waiting, and only a stop (which kills the process) ends it.
+ A tifa execution writes a temporary WAV and a lyrics text file into a scratch directory and removes both when it is over.

## What was measured

A cross-provider measurement on 2026-10-06 (the same material run on both sides, the ONNX side on the CPU execution provider and the ggml side on Vulkan) found the two variants agreeing with their ONNX siblings down to the resolution the contracts carry (10 ms frames):

+ Align: 4 of 6 synthetic samples agreed byte for byte, one differed by a single frame at one boundary, and one (English) differed only in the reading of four polyphonic words; 7 of 11 samples of real singing agreed byte for byte, three differed in 2–4 phoneme boundaries by one frame, and one differed only in the last segment's duration, by 1 µs.
+ Note: onsets differed by at most 2 frames (20 ms) and pitches were mostly equal or one semitone apart. The game variant reproduces exactly in fresh analyzers because it seeds the engine, where the ONNX variant (no seed on `/RandomUniformLike` of `segmenter.onnx`) produced a different number of notes in 5 of 11 samples of real singing across two runs.
+ Ceiling: the note contract's `maxSegmentDuration` differs from the ONNX variant's (30 s against 60 s) and the engine refuses a longer span outright (`this model accepts at most 30.000000 seconds in one execution`).
