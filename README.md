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

+ **Resampling.** Each module declares the sample rate and channel count it needs. They differ between algorithms — RMVPE wants 16 kHz, the GAME note model wants 44.1 kHz — so the host reads the declaration rather than assuming. An executive validates its input and fails on a mismatch; it never resamples behind the caller's back.
+ **Slicing.** The host cuts audio into spans and calls once per span, never exceeding the `maxSegmentDuration` a module declares.
+ **Musical time.** Every time value otter produces is an absolute number of seconds. Ticks, tempo, and tempo curves belong to the host's timeline.

## Why otter must be linked, not loaded

A `SynthUnit` reads the list of registered categories once, when it is constructed. otter registers `analysis` before `main`, so any unit built afterwards has it. A plugin could not do the same: plugins load lazily *through* a unit, so by the time one runs its static initializers, that unit has already built its categories. Contributing a category is something a linked library does.

## Documentation

+ [otter-design.md](docs/otter-design.md) — the design: layering, contract surfaces, host responsibilities, the decision ledger, and the implementation milestones.
