# OTTER: Orchestratable Transcription, Timing and Expression Runtime

An analysis layer for [synthrt](https://github.com/diffscope/synthrt), which adds the `analysis` contribution category to the packages synthrt loads.

synthrt knows about singers and inferences. [wolf](https://github.com/diffscope/wolf) adds what a language is. Neither knows how to look at a recording and say what was sung — which note, at which pitch, at which moment. otter is that layer: a model that reads audio and returns a structured annotation of it is an `analysis` contribution, declared in a package the same way anything else is.

Nothing here is a program. It is a library for an editor or a tool to embed, alongside synthrt.

## The Analysis Contribution

A package listing an analyzer in its `desc.json`:

```json
{
  "contributions": {
    "analysis": [
      { "id": "f0", "path": "./analyzers/f0/analysis.json" }
    ]
  }
}
```

Each entry is a path to an analysis manifest:

```json
{
  "interface": "org.openvpi.analysis.F0",
  "level": 1,
  "variant": "rmvpe",
  "name": "RMVPE",
  "exports": {
    "sampleRate": 16000,
    "channelCount": 1,
    "interval": 0.01,
    "knobs": {
      "voicingThreshold": { "minimum": 0.0, "maximum": 1.0, "default": 0.03 },
      "interpolateUnvoiced": { "default": true }
    }
  },
  "configuration": {
    "model": "./rmvpe.onnx"
  }
}
```

Level 1 defines two contracts:

| `interface` | Produces |
| :-- | :-- |
| `org.openvpi.analysis.F0` | A fundamental frequency curve and a voiced flag per frame |
| `org.openvpi.analysis.Note` | Note intervals, optionally conditioned on notes already known |

Two more names are reserved for contracts that are not written yet: `org.openvpi.analysis.Align` for phoneme and word timings, and `org.openvpi.analysis.Transcribe` for text.

## What otter leaves to the host

otter carries no audio code at all. It does not decode, resample, slice, or read files, and it depends on neither ffmpeg nor libsndfile. An executive takes one contiguous span of float PCM and the time at which that span begins.

That leaves three things for the host:

+ **Resampling.** Each module declares the sample rate it needs. They differ between algorithms — RMVPE wants 16 kHz, the GAME note model wants 44.1 kHz — so the host reads the declaration rather than assuming. An executive refuses audio at another rate; it never resamples behind the caller's back, because that would change the answer without saying so. Channels are the exception and are averaged down, since that is arithmetic with only one possible result.
+ **Slicing.** The host cuts audio into spans and calls once per span, never exceeding the `maxSegmentDuration` a module declares.
+ **Musical time.** Every time value otter produces is an absolute number of seconds. Ticks, tempo, and tempo curves belong to the host's timeline.

## Why otter must be linked, not loaded

A `SynthUnit` reads the list of registered categories once, when it is constructed. otter registers `analysis` before `main`, so any unit built afterwards has it. A plugin could not do the same: plugins load lazily *through* a unit, so by the time one runs its static initializers, that unit has already built its categories. Contributing a category is something a linked library does.

## Versioning and ABI

**otter makes no ABI promise before 1.0.** Almost everything a host touches is a value type in a
public header — every payload under `Api/`, and the contract interfaces change their vtable when a
virtual is added. Mixing objects compiled against two versions of these headers is undefined
behaviour, not a link error, so it will not announce itself. Rebuild the host whenever otter's
version changes.

## Requirements

CMake 3.19 or later, and a checkout of the vcpkg overlay submodule:

```sh
git submodule update --init
```

Every dependency, synthrt included, comes from vcpkg. Ports resolve through two overlays, searched
in order: `scripts/vcpkg-ports` in this repository, then the shared `scripts/vcpkg` submodule.

+ [synthrt](https://github.com/diffscope/synthrt) — via the in-repo `synthrt-main` port, which
  pins the main line
+ [stdcorelib](https://github.com/SineStriker/stdcorelib)
+ [qmsetup](https://github.com/stdware/qmsetup)

Boost.Test is needed only to build the tests.

Both shipped providers run ONNX models and are built only where dsinfer is present. Add the `onnx`
feature (`--x-feature=onnx`) to bring in `synthrt-main[onnx]`. Without it the library still builds,
the category still registers, and packages still load — the same graceful degradation synthrt gives
its own driver.

## Setup Environment

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

The model-backed cases need fixtures, which are generated rather than committed:

```sh
python3 scripts/make-model-fixtures.py --output build/fixtures
```

They are real ONNX graphs with the real signatures and arithmetic for weights. They prove the
providers hold up the contract; they say nothing about whether the numbers are right, which needs
the trained models. Without them those cases skip.

Before publishing a package, lint it:

```sh
python3 scripts/check-declarations.py <package directory>...
```

## How to Use

```cmake
find_package(otter CONFIG REQUIRED)
target_link_libraries(example otter::otter)
```

Linking otter is what registers the category, so an executable that only wants packages with
analysis contributions to load still links it directly rather than relying on a transitive
dependency being pulled in.

## Documentation

+ [otter-design.md](docs/otter-design.md) — the design: layering, contract surfaces, host responsibilities, the decision ledger, the audit, and the implementation milestones.
+ [lite-integration.md](docs/lite-integration.md) — how ds-editor-lite consumes this, and what it has to stop doing itself.
