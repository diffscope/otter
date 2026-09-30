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

+ **Resampling.** Each module declares the sample rate it requires. The rates differ between algorithms (RMVPE requires 16 kHz; the GAME note model and the HFA aligner require 44.1 kHz), so the host reads the declaration instead of assuming a rate. An executive rejects audio at any other rate and never resamples implicitly, because implicit resampling would change the result without notice. Channels are the exception: an executive averages them down, because the downmix has only one possible result.
+ **Slicing.** The host divides the audio into spans and makes one call per span, and no span exceeds the `maxSegmentDuration` that the module declares.
+ **Musical time.** Every time value that otter produces is an absolute number of seconds. Ticks, tempo and tempo curves belong to the host timeline.

## Analyzer creation

A host passes the otter plugin directory, `${OTTER_PLUGINS_DIR}/inferenceinterpreters`, together with the directories of dsinfer and wolf to `SynthUnit::setPluginPaths` for the inference category. The call replaces the existing list, and all directories must therefore be passed in a single call. An analyzer does not require an importing module: the host opens the package, obtains the module, and creates an analyzer through the contract header.

```cpp
auto spec = package.contribution(srt::InferenceCategory::NAME, "f0");
auto analyzer = otter::Api::F0::L1::createAnalyzer(*spec->as<srt::InferenceSpec>());
```

`createAnalyzer` builds the import and runtime options of the contract from the variant of the module and calls `srt::InferenceSpec::createInference`, which rejects a module of another contract before creating an inference. `createAnalyzer` then verifies that the returned executive implements the contract and rejects an executive that does not with `otter::AnalysisError::Internal`. Another module can also import an analyzer through an ordinary `imports` entry with empty `options`.

## Versioning and ABI

**otter provides no ABI stability guarantee before 1.0.** Nearly every type that a host uses is a
value type in a public header: every payload under `Api/` is one, and the vtable of a contract
interface changes when a virtual function is added. Mixing objects compiled against two versions
of these headers is undefined behaviour, not a link error, and therefore produces no diagnostic.
Rebuild the host whenever the otter version changes.

The headers under `otter/Support` are not part of the API. They declare the manifest readers
shared by the library and its plugins, are not installed, and are compiled into a private static
library instead of being exported from the shared library.

## Requirements

CMake 3.19 or later, and a checkout of the vcpkg overlay submodule:

```sh
git submodule update --init
```

Every dependency, synthrt included, comes from vcpkg. Ports resolve through two overlays, searched
in order: `scripts/vcpkg-ports` in this repository, then the shared `scripts/vcpkg` submodule.

+ [synthrt](https://github.com/diffscope/synthrt), through the in-repo `synthrt-main` port, which
  pins the `onnxruntime-builds-uptake` branch of the main line
+ [stdcorelib](https://github.com/SineStriker/stdcorelib)
+ [qmsetup](https://github.com/stdware/qmsetup)

Boost.Test is needed only to build the tests.

All three shipped providers run ONNX models and are built only if dsinfer is present. The `onnx`
feature (`--x-feature=onnx`) adds `synthrt-main[onnx]`, which installs the dsinfer CMake package
beside the synthrt package so that `find_package(dsinfer)` locates it. Without this feature the
library still builds and packages still load; only analyzer execution is unavailable. synthrt
behaves in the same way without its own driver.

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

The model tests (`test_Rmvpe`, `test_Game` and `test_Hfa`) require fixtures, which are generated
instead of committed. If the build finds a Python interpreter with the `onnx` and `numpy` modules,
it generates the fixtures into the build tree as a dependency of those tests; otherwise the three
tests are registered as disabled and ctest reports them as such. The script can also be run
manually:

```sh
python3 scripts/make-model-fixtures.py --output build/fixtures
```

The fixtures are real ONNX graphs with the real signatures and arithmetic in place of trained
weights. They verify that the providers conform to the contract; they do not verify numerical
accuracy, which requires the trained models. A test that finds no fixture, no driver plugin or no
ONNX Runtime at run time exits with the status that ctest reports as skipped, never as passed.

## Model packages

The declarations of the three shipped variants are kept under [packages](packages) without their
model files. Release `models-v0.3.0.0` provides the packages `otter-rmvpe`, `otter-game` and
`otter-hfa`, version 0.2.0.0, together with a `manifest.json`; `scripts/make-package.py`
assembles each package from its declaration and the model files. [packages/README.md](packages/README.md)
describes the assembly.

Every package must pass the lint before publication:

```sh
python3 scripts/check-declarations.py <package directory>...
```

The declarations under `packages/` are kept without their models. `--declarations-only` checks
the declarations without opening the files they refer to; CI checks `packages/` in this mode.

## Usage

```cmake
find_package(otter CONFIG REQUIRED)
target_link_libraries(example otter::otter)
```

The analyzers are plugins located through the inference search path. A host therefore links otter
only for the contract headers and the library functions that they call; linking performs no
registration.

## Documentation

+ [otter-design.md](docs/otter-design.md): the design, including the layering, the contract surfaces, the host responsibilities, the decision ledger, the audits and the implementation milestones.
+ [lite-integration.md](docs/lite-integration.md): the integration of otter into ds-editor-lite and the functions that ds-editor-lite no longer implements itself.
+ [plans/hfa-align.md](docs/plans/hfa-align.md): the implementation plan and record of the Align contract and the `hfa` variant.
+ [packages/README.md](packages/README.md): the declarations of the shipped variants and the package assembly.
