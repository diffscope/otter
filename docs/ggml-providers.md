# The ggml providers

The shipped variants run ONNX models through dsinfer. Two more variants exist beside them: `game-ggml` and `tifa-ggml` drive the ggml engines of [game.cpp](https://github.com/KakaruHayate/game.cpp) and [tifa.cpp](https://github.com/KakaruHayate/tifa.cpp) **as external processes**. They serve the same Level 1 contracts the ONNX variants serve — `org.openvpi.otter.inference.Note` and `org.openvpi.otter.inference.Align` — so a host offers both under one settings page, and a package picks its backend by naming one variant or the other in its manifest.

The provider never touches the engine's weights or the ggml backends: the package names the engine's command line tool, and the provider launches it, hands it one span and reads the answer back. The game engine runs as one long-lived `serve` process per analyzer (a binary request frame on its standard input, one JSON line of notes on its standard output); the tifa engine runs one `align` command per execution and answers with a TextGrid. Everything the engine needs — its executable, the ggml DLLs of its release, the weights, the dictionaries — stays outside the host process, exactly where the engine's own release puts them.

## Installing an engine

The engines ship as release archives the host installs the way it installs the model packages of [docs/packages.md](packages.md): unpack the archive anywhere and name the unpacked files from the declaration.

+ **game.cpp**: unpack the release's `game_ggml-<platform>.oudep` (a zip), which holds `game_ggml_cli.exe`, `game_medium.gguf`, the ggml DLLs and `oudep.yaml`.
+ **tifa.cpp**: unpack the release's `tifa-cli-<platform>` archive (or the `.oudep`), which holds `tifa_ggml_cli.exe`, the `models/` directory with the GGUF weights, the G2P dictionaries and `cpp_pinyin/`.

The configuration block of a declaration names those files, relative to the declaration itself:

| Variant | Contract | `configuration` |
| :-- | :-- | :-- |
| `game-ggml` | `Note` | `cli` (the engine's executable), `model` (the GGUF), `languages` (identifier → model numbering), `timestep` |
| `tifa-ggml` | `Align` | `cli`, `model`, `dictionaries` (the G2P dictionary tree), `languages` (identifier → engine code) |

Two example declarations, written against real releases, live under [docs/examples/packages](examples/packages): `game-ggml` (GAME-1.0-medium) and `tifa-ggml` (TIFA-1.0-ST). They pass the lint as they are, and `make-package.py` assembles them the way it assembles the factory packages — point `--models` at the unpacked release archive and the packager copies the engine, the model and the dictionaries into the package. Their `version` and `compatVersion` are pinned to the project version of the tree that carries them, as the lint requires of every declaration, so a project version bump must bump them with it; the lint names the file that lags.

```sh
python3 scripts/make-package.py --variant game-ggml \
    --declarations docs/examples/packages \
    --models /path/to/unpacked/game_ggml-windows-x64-vulkan-q8 \
    --manifest build/packages/manifest.json
```

The engine's own runtime libraries (the `ggml*.dll` files beside the tool on Windows) are what the tool loads at run time, and an archive assembled from the declaration alone does not name them. The simplest install is therefore to unpack the release archive first and put the declaration into it: copy `desc.json` and the `inferences/` directory of the assembled package into the unpacked archive, and the whole directory is at once an engine install and an otter package.

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

+ The engine call is monolithic: progress reports 0 and then 1. A stop, however, kills the engine process — the game variant's next execution relaunches it and pays the model load again, and a tifa execution reports the cancellation as soon as its tool is gone.
+ `game-ggml` carries no alignment path, because the serve protocol runs no session for the model that converts known durations into boundaries, so its exports cannot declare `supportsKnownNotes`, and an execution that supplies known notes is refused.
+ `tifa-ggml` aligns the first pronunciation the G2P offers for a polyphonic word, where the ONNX variant scores the readings against the audio and picks one. A caller who needs another reading writes the lyrics as the scheme spelling that dictionary entry carries.
+ `game-ggml` seeds the engine's sampling randomness with a fixed value, so repeated transcriptions of one span produce one transcription; the engine treats seed zero as a request for a random one.
+ The engine's own progress notes go to the host's standard error, which a GUI host may want to capture.
