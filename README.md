# OTTER: Orchestratable Transcription, Timing and Expression Runtime

An analysis layer for [synthrt](https://github.com/diffscope/synthrt) that defines contracts and interpreters for inference modules producing structured annotations of audio.

synthrt defines singers and inferences, and [wolf](https://github.com/diffscope/wolf) adds languages. Neither library determines the sung content of a recording, that is, the notes, their pitches and their timing. otter provides this layer. A model that reads audio and returns a structured annotation of it is an inference module whose interface is one of the otter contracts, and a package declares it in the same way as any other module. otter adds no contribution category; the analyzers belong to the built-in synthrt `inference` category, as the wolf G2P and S2P modules do.

otter contains no executable. It is a library that an editor or a tool embeds alongside synthrt.

## The Analysis Module

A package listing an analyzer in its `desc.json`:

```json
{
  "contributions": {
    "inference": [
      { "id": "f0", "path": "./inferences/f0/inference.json" }
    ]
  }
}
```

Each entry is a path to an analysis manifest:

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

The two blocks follow the division defined by spec 2.4. `exports` belongs to the contract: it declares the audio format that a host must prepare and the knobs that the module supports, has the same form for every variant of the interface, and is published as JSON Schema under [docs/schemas](docs/schemas). `configuration` belongs to the variant: it specifies the model paths and parameters, and no host reads it. A variant checks the two blocks against each other at load. A package that declares a sample rate its model does not support, or an alignment path without the model that performs it, is therefore rejected before any analysis runs.

Level 1 defines three contracts:

| `interface` | Produces |
| :-- | :-- |
| `org.openvpi.otter.inference.F0` | A fundamental frequency curve and a voiced flag per frame |
| `org.openvpi.otter.inference.Note` | Note intervals, optionally conditioned on notes already known |
| `org.openvpi.otter.inference.Align` | Word and phoneme intervals, for lyrics the caller already knows |

The name `org.openvpi.otter.inference.Transcribe` is reserved for a future text transcription contract.

## Host responsibilities

otter contains no audio processing code. It does not decode, resample, slice or read files, and it depends on neither ffmpeg nor libsndfile. An executive accepts one contiguous span of float PCM and the start time of that span.

The host is therefore responsible for three tasks:

+ **Resampling.** Each module declares the sample rate it requires. The rates differ between algorithms (RMVPE requires 16 kHz; the GAME note model and the HFA aligner require 44.1 kHz, the TIFA aligner 48 kHz), so the host reads the declaration instead of assuming a rate. An executive rejects audio at any other rate and never resamples implicitly, because implicit resampling would change the result without notice. Channels are the exception: an executive averages them down, because the downmix has only one possible result.
+ **Slicing.** The host divides the audio into spans and makes one call per span, and no span exceeds the `maxSegmentDuration` that the module declares.
+ **Musical time.** Every time value that otter produces is an absolute number of seconds. Ticks, tempo and tempo curves belong to the host timeline.

## Analyzer creation

A host passes the otter plugin directory, `${OTTER_PLUGINS_DIR}/inferenceinterpreters`, together with the directories of dsinfer and wolf to `SynthUnit::setPluginPaths` for the inference category. The call replaces the existing list, and all directories must therefore be passed in a single call. An analyzer does not require an importing module: the host opens the package, obtains the module, and creates an analyzer through the contract header.

```cpp
auto spec = package.contribution(srt::InferenceCategory::NAME, "f0");
auto analyzer = otter::Api::F0::L1::createAnalyzer(*spec->as<srt::InferenceSpec>());
```

`createAnalyzer` builds the import and runtime options of the contract from the variant of the module and calls `srt::InferenceSpec::createInference`, which rejects a module of another contract before creating an inference. `createAnalyzer` then verifies that the returned executive implements the contract and rejects an executive that does not with `otter::AnalysisError::ContractViolation`. Another module can also import an analyzer through an ordinary `imports` entry with empty `options`.

## Versioning and ABI

**otter provides no ABI stability guarantee before 1.0.** Nearly every type that a host uses is a value type in a public header: every payload under `Api/` is one, and the vtable of a contract interface changes when a virtual function is added. Mixing objects compiled against two versions of these headers is undefined behaviour, not a link error, and therefore produces no diagnostic. Rebuild the host whenever the otter version changes.

The headers under `otter/Support` are not part of the API. They declare the manifest readers shared by the library and its plugins, are not installed, and are compiled into a private static library instead of being exported from the shared library.

## Requirements

CMake 3.20 or later (configuring needs the 3.19 the project declares; the `ctest --test-dir` used below needs 3.20), and a checkout of the vcpkg overlay submodule:

```sh
git submodule update --init
```

Every dependency, synthrt included, comes from vcpkg. Ports resolve through two overlays, searched in order: `scripts/vcpkg-ports` in this repository, then the shared `scripts/vcpkg` submodule.

+ [synthrt](https://github.com/diffscope/synthrt), through the in-repo `synthrt-main` port, which pins the `onnxruntime-builds-uptake` branch of the main line
+ [stdcorelib](https://github.com/SineStriker/stdcorelib)
+ [qmsetup](https://github.com/stdware/qmsetup)

Boost.Test is needed only to build the tests.

All four shipped providers run ONNX models and are built only if dsinfer is present. The `onnx` feature (`--x-feature=onnx`) adds `synthrt-main[onnx]`, which installs the dsinfer CMake package beside the synthrt package so that `find_package(dsinfer)` locates it. Without this feature the library still builds and packages still load; only analyzer execution is unavailable. synthrt behaves in the same way without its own driver.

## Build

```sh
vcpkg install --x-manifest-root=scripts/vcpkg-manifest --x-install-root=build/vcpkg_installed \
    --x-feature=onnx --x-feature=tests

cmake -B build/cmake -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
    -DVCPKG_INSTALLED_DIR=<install root passed above> \
    -DVCPKG_MANIFEST_MODE=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DOTTER_BUILD_TESTS=ON

cmake --build build/cmake
ctest --test-dir build/cmake
```

The model tests (`test_Rmvpe`, `test_Game`, `test_Hfa` and `test_Tifa`) require fixtures, which are generated instead of committed. If the build finds a Python interpreter with the `onnx` and `numpy` modules, it generates the fixtures into the build tree as a dependency of those tests; otherwise the four tests are registered as disabled and ctest reports them as such. That decision is taken while configuring; a test that is enabled but finds its data missing at run time reports itself as skipped instead, which is a different mechanism (described below, together with the same distinction for the loading tests). The script can also be run manually:

```sh
python3 scripts/make-model-fixtures.py --output build/cmake/fixtures
```

The fixtures are real ONNX graphs with the real signatures and arithmetic in place of trained weights. They verify that the providers conform to the contract; they do not verify numerical accuracy, which requires the trained models. A test that finds no fixture, no driver plugin or no ONNX Runtime at run time exits with the status that ctest reports as skipped, never as passed.

The loading and runtime tests (`test_AnalysisLoad` and `test_AnalysisRuntime`) read a directory of packages that is not committed either. `scripts/make-test-packages.cmake` holds their contents, and one command prepares them:

```sh
cmake -DOTTER_TEST_PACKAGES_OUTPUT=build/test-packages -P scripts/make-test-packages.cmake
```

Pass the directory to the build so the tests find it, and the same command with `-DOTTER_TEST_PACKAGES_CHECK=<dir>` compares an existing directory against the script's table, reporting every file that is missing, differs or is unexpected:

```sh
cmake -B build/cmake -G Ninja \
    ... \
    -DOTTER_TEST_PACKAGES_SOURCE="$PWD/build/test-packages"
```

Without that variable the two tests exit with the status ctest reports as skipped; with it naming a directory that does not exist they fail, so a checkout that was told where the packages are and finds none cannot pass unnoticed. Note that ctest returns zero for a run whose tests skipped: it prints how many did not run at the end, but the exit status stays green, so that count is the only sign and CI must not read the status alone. Generating the directory and checking it, as CI does before configuring, is what keeps a green run from hiding tests that never ran.

## Model packages

A package is a declaration plus the model files it names, and it travels as one archive that is published with the models rather than from this repository: release `models-v0.1` provides `otter-game`, `otter-hfa`, `otter-rmvpe` and `otter-tifa`, version 0.1.0.0, with a `manifest.json` that lists all four. The declarations are not tracked here; assembly reads them from the directory given as `--declarations`, and [docs/packages.md](docs/packages.md) is the authority on what a release contains. Two more variants run the engines of their own releases instead of ONNX graphs and package an engine archive rather than weights; [docs/ggml-providers.md](docs/ggml-providers.md) describes them and the declarations under [docs/examples/packages](docs/examples/packages).

A checkout can be given the real packages with one command, without a Python interpreter:

```sh
DOTTER_FETCH_OUTPUT=build/models DOTTER_FETCH_VARIANTS=hfa \
    cmake -P scripts/fetch-models.cmake
```

The script reads the manifest of the release, checks every archive against the SHA512 that manifest records for it, and unpacks what matches. Stating no variants only lists what the release holds, because a release is hundreds of megabytes; `DOTTER_FETCH_VARIANTS=all` takes every package, `DOTTER_FETCH_TAG` reads another release, `DOTTER_FETCH_KEEP_ARCHIVES=ON` keeps the archives beside the packages, and `DOTTER_FETCH_MANIFEST` reads a manifest that is already on disk, which is how a run works offline. Settings are read from the environment first and from cache variables second; the examples use the environment, because an untyped `-D` entry for one of these names was measured to be dropped by cmake 4.3.1.

Assembly copies the declarations, adds the model files and writes the archive, its checksum and a manifest fragment:

```sh
python3 scripts/make-package.py --variant hfa \
    --declarations /path/to/declarations \
    --models /path/to/hubertfa-v0.0.7 --manifest build/packages/manifest.json
```

Every package must pass the lint before publication:

```sh
python3 scripts/check-declarations.py <package directory>...
```

`--declarations-only` checks a declaration without opening the files it refers to, so a packager can run it before the model files are in place.

## Usage

```cmake
find_package(otter CONFIG REQUIRED)
target_link_libraries(example otter::otter)
```

The analyzers are plugins located through the inference search path. A host therefore links otter only for the contract headers and the library functions that they call; linking performs no registration.

## Documentation

+ [otter-design.md](docs/otter-design.md): the design, including the layering, the contract surfaces, the host responsibilities, the decision ledger, the audits and the implementation milestones.
+ [packages.md](docs/packages.md): the factory declarations, the assembly of a package and the checks it must pass.
+ [ggml-providers.md](docs/ggml-providers.md): the `game-ggml` and `tifa-ggml` variants, which drive the ggml engines of game.cpp and tifa.cpp as external processes.
+ [lite-integration.md](docs/lite-integration.md): the integration of otter into ds-editor-lite and the functions that ds-editor-lite no longer implements itself.
+ [plans/hfa-align.md](docs/plans/hfa-align.md): the implementation plan and record of the Align contract and the `hfa` variant.
+ [plans/tifa-align.md](docs/plans/tifa-align.md): the implementation plan and record of the `tifa` variant, including the readings of the real-weight gate.
