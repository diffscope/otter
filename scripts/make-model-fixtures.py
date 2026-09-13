#!/usr/bin/env python3
"""Builds analysis packages whose models have the real signatures and fake weights.

The shipped providers are judged on whether they hold up the contract: the right tensors go in,
the outputs are read back in the right order, the knobs reach the model, the timings come out
anchored where the host said the audio was. None of that needs a trained model, and a trained one
cannot live in a repository anyway.

So these are real ONNX graphs with the real input and output names, shapes and dtypes, wired to
arithmetic a test can predict. A provider that mixes two inputs up, drops a knob, or reads an
output as the wrong dtype fails here exactly as it would against the real weights.

What they cannot test is whether the numbers mean anything. That needs the real models, and the
milestone that uses them says so.

    python3 scripts/make-model-fixtures.py --output build/fixtures
"""

import argparse
import json
import shutil
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

# RMVPE runs at 16 kHz with a 160 sample hop, which is exactly 10 ms a frame.
RMVPE_RATE = 16000
RMVPE_HOP = 160

# The note model's config declares 44.1 kHz and a 10 ms frame.
NOTE_RATE = 44100
NOTE_TIMESTEP = 0.01


def _sorted(nodes: list, available: set) -> list:
    """Orders nodes so every input is produced before it is read.

    ONNX requires a topologically sorted graph. Doing it here rather than by hand keeps each
    builder readable in the order the computation makes sense, and turns a mis-ordering into a
    loud failure instead of a checker message about one node.
    """
    remaining = list(nodes)
    ordered = []
    ready = set(available)
    while remaining:
        progressed = False
        for node in list(remaining):
            if all(name in ready or not name for name in node.input):
                ordered.append(node)
                ready.update(node.output)
                remaining.remove(node)
                progressed = True
        if not progressed:
            missing = {
                name for node in remaining for name in node.input if name and name not in ready
            }
            raise ValueError(f"nothing produces {sorted(missing)}")
    return ordered


def _save(graph: onnx.GraphProto, path: Path) -> None:
    available = {value.name for value in graph.input}
    available.update(value.name for value in graph.initializer)
    ordered = _sorted(list(graph.node), available)
    del graph.node[:]
    graph.node.extend(ordered)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 10
    onnx.checker.check_model(model)
    path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, str(path))


def _const(name: str, array: np.ndarray) -> onnx.NodeProto:
    return helper.make_node(
        "Constant", [], [name], value=numpy_helper.from_array(array, name + "_value")
    )


def build_rmvpe(path: Path) -> None:
    """waveform [1, samples] + threshold scalar -> f0 [frames] float, uv [frames] bool.

    The curve is a ramp so that a test can tell frame order from frame content, and the voicing
    flag is derived from the threshold so that the knob demonstrably reaches the model. The flag
    is the model's own convention: true marks an *unvoiced* frame, which is the reading the
    provider inverts on the way out.
    """
    waveform = helper.make_tensor_value_info("waveform", TensorProto.FLOAT, [1, "samples"])
    threshold = helper.make_tensor_value_info("threshold", TensorProto.FLOAT, [1])
    f0 = helper.make_tensor_value_info("f0", TensorProto.FLOAT, ["frames"])
    uv = helper.make_tensor_value_info("uv", TensorProto.BOOL, ["frames"])

    nodes = [
        # frames = samples // hop, taken from the waveform's own shape so that a provider handing
        # over the wrong buffer produces a visibly wrong length.
        helper.make_node("Shape", ["waveform"], ["shape"]),
        _const("one", np.array([1], dtype=np.int64)),
        _const("two", np.array([2], dtype=np.int64)),
        helper.make_node("Slice", ["shape", "one", "two"], ["samples"]),
        _const("hop", np.array([RMVPE_HOP], dtype=np.int64)),
        helper.make_node("Div", ["samples", "hop"], ["frames"]),
        # index = [0, 1, ... frames-1]
        _const("zero_i", np.array([0], dtype=np.int64)),
        _const("step_i", np.array([1], dtype=np.int64)),
        helper.make_node("Range", ["zero_i_s", "frames_s", "step_i_s"], ["index"]),
        helper.make_node("Squeeze", ["zero_i", "zero_i_axis"], ["zero_i_s"]),
        _const("zero_i_axis", np.array([0], dtype=np.int64)),
        helper.make_node("Squeeze", ["frames", "zero_i_axis"], ["frames_s"]),
        helper.make_node("Squeeze", ["step_i", "zero_i_axis"], ["step_i_s"]),
        # f0 = 100 + index, a ramp in hertz.
        helper.make_node("Cast", ["index"], ["index_f"], to=TensorProto.FLOAT),
        _const("base", np.array([100.0], dtype=np.float32)),
        helper.make_node("Add", ["index_f", "base"], ["f0"]),
        # uv = (index % 100) >= round(threshold * 100): a higher threshold calls more frames
        # unvoiced, so the knob is visible in the output.
        _const("hundred", np.array([100], dtype=np.int64)),
        helper.make_node("Mod", ["index", "hundred"], ["phase"]),
        _const("scale", np.array([100.0], dtype=np.float32)),
        helper.make_node("Mul", ["threshold", "scale"], ["cut_f"]),
        helper.make_node("Cast", ["cut_f"], ["cut"], to=TensorProto.INT64),
        helper.make_node("GreaterOrEqual", ["phase", "cut"], ["uv"]),
    ]
    _save(helper.make_graph(nodes, "rmvpe", [waveform, threshold], [f0, uv]), path)


def build_note_encoder(path: Path) -> None:
    """waveform [1, samples] + duration [1] -> x_seg, x_est [1, T, C] and maskT [1, T] bool."""
    waveform = helper.make_tensor_value_info("waveform", TensorProto.FLOAT, [1, "samples"])
    duration = helper.make_tensor_value_info("duration", TensorProto.FLOAT, [1])
    x_seg = helper.make_tensor_value_info("x_seg", TensorProto.FLOAT, [1, "T", 4])
    x_est = helper.make_tensor_value_info("x_est", TensorProto.FLOAT, [1, "T", 4])
    maskT = helper.make_tensor_value_info("maskT", TensorProto.BOOL, [1, "T"])

    hop = int(NOTE_RATE * NOTE_TIMESTEP)
    nodes = [
        helper.make_node("Shape", ["waveform"], ["shape"]),
        _const("one", np.array([1], dtype=np.int64)),
        _const("two", np.array([2], dtype=np.int64)),
        helper.make_node("Slice", ["shape", "one", "two"], ["samples"]),
        _const("hop", np.array([hop], dtype=np.int64)),
        helper.make_node("Div", ["samples", "hop"], ["T"]),
        _const("batch", np.array([1], dtype=np.int64)),
        _const("channels", np.array([4], dtype=np.int64)),
        helper.make_node("Concat", ["batch", "T", "channels"], ["feature_shape"], axis=0),
        # The duration is folded into the features so that a provider sending the wrong number
        # cannot pass unnoticed.
        helper.make_node("Expand", ["duration", "feature_shape"], ["x_seg"]),
        helper.make_node("Identity", ["x_seg"], ["x_est"]),
        helper.make_node("Concat", ["batch", "T"], ["mask_shape"], axis=0),
        _const("true_v", np.array([True], dtype=bool)),
        helper.make_node("Expand", ["true_v", "mask_shape"], ["maskT"]),
    ]
    _save(
        helper.make_graph(nodes, "note_encoder", [waveform, duration], [x_seg, x_est, maskT]),
        path,
    )


def build_note_segmenter(path: Path) -> None:
    """Places a boundary every N frames, where N comes from the threshold and the radius.

    Real segmentation is a diffusion loop; this is arithmetic over the same inputs. What it
    preserves is the part a provider can get wrong: every declared input must arrive, the knobs
    must change the answer, and the shapes must be the ones a real export declares.

    That last one is why `t` is `[B]` and not `["steps"]`, and why the two knobs are scalars.
    An earlier version of this fixture wrote the shapes the provider happened to send, so the
    provider was being checked against its own assumptions and passed. Against a real GAME export
    it failed on the first call: every segmenter input shares one batch dimension, `t` included,
    so a call carries one timestep and the sampling loop belongs to the caller.
    """
    x_seg = helper.make_tensor_value_info("x_seg", TensorProto.FLOAT, [1, "T", 4])
    maskT = helper.make_tensor_value_info("maskT", TensorProto.BOOL, [1, "T"])
    known = helper.make_tensor_value_info("known_boundaries", TensorProto.BOOL, [1, "T"])
    previous = helper.make_tensor_value_info("prev_boundaries", TensorProto.BOOL, [1, "T"])
    language = helper.make_tensor_value_info("language", TensorProto.INT64, [1])
    threshold = helper.make_tensor_value_info("threshold", TensorProto.FLOAT, [])
    radius = helper.make_tensor_value_info("radius", TensorProto.INT64, [])
    t = helper.make_tensor_value_info("t", TensorProto.FLOAT, [1])
    boundaries = helper.make_tensor_value_info("boundaries", TensorProto.BOOL, [1, "T"])

    nodes = [
        helper.make_node("Shape", ["maskT"], ["shape"]),
        _const("one", np.array([1], dtype=np.int64)),
        _const("two", np.array([2], dtype=np.int64)),
        _const("axis", np.array([0], dtype=np.int64)),
        helper.make_node("Slice", ["shape", "one", "two"], ["T"]),
        helper.make_node("Squeeze", ["T", "axis"], ["T_s"]),
        _const("zero_i", np.array(0, dtype=np.int64)),
        _const("step_i", np.array(1, dtype=np.int64)),
        helper.make_node("Range", ["zero_i", "T_s", "step_i"], ["index"]),
        # period = max(radius, 10): the radius knob widens the notes. radius arrives as a scalar,
        # so it is lifted to a one element vector first.
        _const("floor_p", np.array([10], dtype=np.int64)),
        helper.make_node("Unsqueeze", ["radius", "axis"], ["radius_1"]),
        helper.make_node("Max", ["radius_1", "floor_p"], ["period"]),
        helper.make_node("Mod", ["index", "period_b"], ["phase"]),
        helper.make_node("Reshape", ["period", "one"], ["period_b"]),
        _const("zero_b", np.array([0], dtype=np.int64)),
        helper.make_node("Equal", ["phase", "zero_b"], ["at_period"]),
        # A boundary the caller already knows always stays one, which is the alignment path.
        helper.make_node("Or", ["at_period_1", "known_boundaries"], ["with_known"]),
        helper.make_node("Unsqueeze", ["at_period", "axis"], ["at_period_1"]),
        helper.make_node("And", ["with_known", "maskT"], ["boundaries"]),
        # Every remaining declared input is consumed, so a provider that omits one fails to run.
        helper.make_node("ReduceSum", ["t"], ["t_sum"], keepdims=0),
        helper.make_node("Cast", ["language"], ["language_f"], to=TensorProto.FLOAT),
        helper.make_node("Unsqueeze", ["threshold", "axis"], ["threshold_1"]),
        helper.make_node("Add", ["threshold_1", "language_f"], ["knobs"]),
        helper.make_node("ReduceSum", ["x_seg"], ["x_sum"], keepdims=0),
        helper.make_node("Cast", ["prev_boundaries"], ["previous_f"], to=TensorProto.FLOAT),
        helper.make_node("ReduceSum", ["previous_f"], ["previous_sum"], keepdims=0),
    ]
    _save(
        helper.make_graph(
            nodes,
            "note_segmenter",
            [x_seg, maskT, known, previous, language, threshold, radius, t],
            [boundaries],
        ),
        path,
    )


def build_note_bd2dur(path: Path) -> None:
    """boundaries [1, T] bool + maskT [1, T] bool -> durations [1, N] seconds, maskN [1, N] bool.

    Each note runs from one boundary to the next, so N is the number of boundaries and every
    duration is the period in seconds.
    """
    boundaries = helper.make_tensor_value_info("boundaries", TensorProto.BOOL, [1, "T"])
    maskT = helper.make_tensor_value_info("maskT", TensorProto.BOOL, [1, "T"])
    durations = helper.make_tensor_value_info("durations", TensorProto.FLOAT, [1, "N"])
    maskN = helper.make_tensor_value_info("maskN", TensorProto.BOOL, [1, "N"])

    nodes = [
        helper.make_node("Cast", ["boundaries"], ["b_i"], to=TensorProto.INT64),
        helper.make_node("ReduceSum", ["b_i"], ["N"], keepdims=1),
        _const("one", np.array([1], dtype=np.int64)),
        helper.make_node("Reshape", ["N", "one"], ["N_flat"]),
        helper.make_node("Concat", ["one", "N_flat"], ["out_shape"], axis=0),
        # Every note lasts one period; the period is recovered from T / N.
        helper.make_node("Shape", ["maskT"], ["shape"]),
        _const("two", np.array([2], dtype=np.int64)),
        helper.make_node("Slice", ["shape", "one", "two"], ["T"]),
        helper.make_node("Div", ["T", "N_flat"], ["period"]),
        helper.make_node("Cast", ["period"], ["period_f"], to=TensorProto.FLOAT),
        _const("timestep", np.array([NOTE_TIMESTEP], dtype=np.float32)),
        helper.make_node("Mul", ["period_f", "timestep"], ["one_duration"]),
        helper.make_node("Expand", ["one_duration", "out_shape"], ["durations"]),
        _const("true_v", np.array([True], dtype=bool)),
        helper.make_node("Expand", ["true_v", "out_shape"], ["maskN"]),
    ]
    _save(
        helper.make_graph(nodes, "note_bd2dur", [boundaries, maskT], [durations, maskN]), path
    )


def build_note_dur2bd(path: Path) -> None:
    """durations [1, N] seconds + maskT [1, T] bool -> boundaries [1, T] bool.

    The alignment path: known note durations become the boundaries the segmenter is conditioned
    on. A boundary is placed at the cumulative start of every note.
    """
    durations = helper.make_tensor_value_info("durations", TensorProto.FLOAT, [1, "N"])
    maskT = helper.make_tensor_value_info("maskT", TensorProto.BOOL, [1, "T"])
    boundaries = helper.make_tensor_value_info("boundaries", TensorProto.BOOL, [1, "T"])

    nodes = [
        _const("axis", np.array([0], dtype=np.int64)),
        _const("one", np.array([1], dtype=np.int64)),
        _const("two", np.array([2], dtype=np.int64)),
        helper.make_node("Shape", ["maskT"], ["shape"]),
        helper.make_node("Slice", ["shape", "one", "two"], ["T"]),
        helper.make_node("Squeeze", ["T", "axis"], ["T_s"]),
        _const("zero_i", np.array(0, dtype=np.int64)),
        _const("step_i", np.array(1, dtype=np.int64)),
        helper.make_node("Range", ["zero_i", "T_s", "step_i"], ["index"]),
        # starts = cumulative sum of the durations, less the last, converted to frames.
        _const("cum_axis", np.array(1, dtype=np.int64)),
        helper.make_node("CumSum", ["durations", "cum_axis"], ["ends"], exclusive=1),
        _const("timestep", np.array([NOTE_TIMESTEP], dtype=np.float32)),
        helper.make_node("Div", ["ends", "timestep"], ["start_frames_f"]),
        helper.make_node("Cast", ["start_frames_f"], ["start_frames"], to=TensorProto.INT64),
        helper.make_node("Unsqueeze", ["index", "axis"], ["index_1"]),
        helper.make_node("Unsqueeze", ["start_frames", "minus_one"], ["starts_col"]),
        _const("minus_one", np.array([-1], dtype=np.int64)),
        helper.make_node("Unsqueeze", ["index_1", "one"], ["index_row"]),
        helper.make_node("Equal", ["index_row", "starts_col"], ["hits"]),
        helper.make_node("Cast", ["hits"], ["hits_i"], to=TensorProto.INT64),
        _const("sum_axis", np.array([1], dtype=np.int64)),
        helper.make_node("ReduceSum", ["hits_i", "sum_axis"], ["hit_count"], keepdims=0),
        _const("zero_c", np.array([0], dtype=np.int64)),
        helper.make_node("Greater", ["hit_count", "zero_c"], ["marked"]),
        helper.make_node("And", ["marked", "maskT"], ["boundaries"]),
    ]
    _save(helper.make_graph(nodes, "note_dur2bd", [durations, maskT], [boundaries]), path)


def build_note_estimator(path: Path) -> None:
    """x_est, boundaries, maskT, maskN, threshold -> presence [1, N], scores [1, N] MIDI keys."""
    x_est = helper.make_tensor_value_info("x_est", TensorProto.FLOAT, [1, "T", 4])
    boundaries = helper.make_tensor_value_info("boundaries", TensorProto.BOOL, [1, "T"])
    maskT = helper.make_tensor_value_info("maskT", TensorProto.BOOL, [1, "T"])
    maskN = helper.make_tensor_value_info("maskN", TensorProto.BOOL, [1, "N"])
    threshold = helper.make_tensor_value_info("threshold", TensorProto.FLOAT, [])
    presence = helper.make_tensor_value_info("presence", TensorProto.BOOL, [1, "N"])
    scores = helper.make_tensor_value_info("scores", TensorProto.FLOAT, [1, "N"])

    nodes = [
        helper.make_node("Shape", ["maskN"], ["shape"]),
        _const("one", np.array([1], dtype=np.int64)),
        _const("two", np.array([2], dtype=np.int64)),
        _const("axis", np.array([0], dtype=np.int64)),
        helper.make_node("Slice", ["shape", "one", "two"], ["N"]),
        helper.make_node("Squeeze", ["N", "axis"], ["N_s"]),
        _const("zero_i", np.array(0, dtype=np.int64)),
        _const("step_i", np.array(1, dtype=np.int64)),
        helper.make_node("Range", ["zero_i", "N_s", "step_i"], ["index"]),
        helper.make_node("Cast", ["index"], ["index_f"], to=TensorProto.FLOAT),
        # A chromatic run from middle C, so note order is visible in the pitches.
        _const("middle_c", np.array([60.0], dtype=np.float32)),
        helper.make_node("Add", ["index_f", "middle_c"], ["scores_flat"]),
        helper.make_node("Unsqueeze", ["scores_flat", "axis"], ["scores"]),
        # presence alternates present / absent so that a cutoff still has something to cut. It is
        # a boolean because that is what the shipped model writes: whether a note is there, rather
        # than how sure it is. A host reads it as a confidence of one or zero.
        _const("hundred", np.array([2], dtype=np.int64)),
        helper.make_node("Mod", ["index", "hundred"], ["parity"]),
        _const("zero_p", np.array([0], dtype=np.int64)),
        helper.make_node("Equal", ["parity", "zero_p"], ["presence_flat"]),
        helper.make_node("Unsqueeze", ["presence_flat", "axis"], ["presence"]),
        _const("high", np.array([0.9], dtype=np.float32)),
        # Consume the remaining declared inputs.
        helper.make_node("ReduceSum", ["x_est"], ["x_sum"], keepdims=0),
        helper.make_node("Cast", ["boundaries"], ["b_f"], to=TensorProto.FLOAT),
        helper.make_node("ReduceSum", ["b_f"], ["b_sum"], keepdims=0),
        helper.make_node("Cast", ["maskT"], ["m_f"], to=TensorProto.FLOAT),
        helper.make_node("ReduceSum", ["m_f"], ["m_sum"], keepdims=0),
        helper.make_node("Add", ["threshold", "high"], ["threshold_used"]),
    ]
    _save(
        helper.make_graph(
            nodes,
            "note_estimator",
            [x_est, boundaries, maskT, maskN, threshold],
            [presence, scores],
        ),
        path,
    )


def write_package(root: Path, identifier: str, contributions: dict) -> None:
    desc = {
        "$version": "1.0",
        "id": identifier,
        "version": "1.0.0.0",
        # A floor of its own version is the honest claim for a first release: nothing older exists
        # to be compatible with. It is stated rather than omitted so that the next release only has
        # to move it, not introduce it.
        "compatVersion": "1.0.0.0",
        "runtimeLevel": 1,
        "contributions": {
            "analysis": [
                {"id": name, "path": f"./analyzers/{name}/analysis.json"}
                for name in contributions
            ]
        },
    }
    (root / "desc.json").write_text(json.dumps(desc, indent=4) + "\n", encoding="utf-8")
    for name, declaration in contributions.items():
        directory = root / "analyzers" / name
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "analysis.json").write_text(
            json.dumps(declaration, indent=4) + "\n", encoding="utf-8"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default="build/fixtures", type=Path)
    args = parser.parse_args()

    output = args.output
    if output.exists():
        shutil.rmtree(output)

    rmvpe = output / "fixture-rmvpe"
    build_rmvpe(rmvpe / "analyzers" / "f0" / "rmvpe.onnx")
    write_package(
        rmvpe,
        "otter/fixture-rmvpe",
        {
            "f0": {
                "interface": "org.openvpi.otter.analysis.F0",
                "level": 1,
                "variant": "rmvpe",
                "name": "Fixture RMVPE",
                # The contract facts a host reads: the audio format and the knobs. The syntax
                # belongs to the interface, so a declaration of any variant reads the same way.
                "exports": {
                    "sampleRate": RMVPE_RATE,
                    "channelCount": 1,
                    "interval": 0.01,
                    "maxSegmentDuration": 60.0,
                    "knobs": {
                        "voicingThreshold": {"minimum": 0.0, "maximum": 1.0, "default": 0.03},
                        "interpolateUnvoiced": {"default": True},
                    },
                },
                # What only the rmvpe variant reads: where its model is.
                "configuration": {
                    "model": "./rmvpe.onnx",
                },
            }
        },
    )

    note = output / "fixture-note"
    models = note / "analyzers" / "note"
    build_note_encoder(models / "encoder.onnx")
    build_note_segmenter(models / "segmenter.onnx")
    build_note_estimator(models / "estimator.onnx")
    build_note_bd2dur(models / "bd2dur.onnx")
    build_note_dur2bd(models / "dur2bd.onnx")
    write_package(
        note,
        "otter/fixture-note",
        {
            "note": {
                "interface": "org.openvpi.otter.analysis.Note",
                "level": 1,
                "variant": "game",
                "name": "Fixture Note",
                "exports": {
                    "sampleRate": NOTE_RATE,
                    "channelCount": 1,
                    "maxSegmentDuration": 60.0,
                    "languages": ["zxx", "zh"],
                    "supportsKnownNotes": True,
                    "knobs": {
                        "boundaryThreshold": {"minimum": 0.0, "maximum": 1.0, "default": 0.2},
                        "boundaryRadius": {"minimum": 0.0, "maximum": 1.0, "default": 0.2},
                        "noteThreshold": {"minimum": 0.0, "maximum": 1.0, "default": 0.2},
                        "notePresenceCutoff": {"minimum": 0.0, "maximum": 1.0, "default": 0.5},
                        "steps": {"minimum": 1, "maximum": 1000, "default": 8},
                    },
                },
                # What only the game variant reads: its five models and how they are wired.
                "configuration": {
                    "encoder": "./encoder.onnx",
                    "segmenter": "./segmenter.onnx",
                    "estimator": "./estimator.onnx",
                    "boundaryToDuration": "./bd2dur.onnx",
                    "durationToBoundary": "./dur2bd.onnx",
                    "timestep": NOTE_TIMESTEP,
                    "languages": {"zxx": 0, "zh": 1},
                    "defaultLanguage": "zxx",
                },
            }
        },
    )

    # Declarations the variants must refuse at load: the contract syntax is fine, but the package
    # promises what its models cannot honor. Each gets graphs of its own so that the refusal is
    # about the declaration and not about a missing file.
    wrong_rate = output / "fixture-rmvpe-wrong-rate"
    build_rmvpe(wrong_rate / "analyzers" / "f0" / "rmvpe.onnx")
    write_package(
        wrong_rate,
        "otter/fixture-rmvpe-wrong-rate",
        {
            "f0": {
                "interface": "org.openvpi.otter.analysis.F0",
                "level": 1,
                "variant": "rmvpe",
                "name": "Fixture RMVPE at the wrong rate",
                "exports": {"sampleRate": 44100, "interval": 0.01},
                "configuration": {"model": "./rmvpe.onnx"},
            }
        },
    )

    def note_models(directory: Path) -> dict:
        build_note_encoder(directory / "encoder.onnx")
        build_note_segmenter(directory / "segmenter.onnx")
        build_note_estimator(directory / "estimator.onnx")
        build_note_bd2dur(directory / "bd2dur.onnx")
        return {
            "encoder": "./encoder.onnx",
            "segmenter": "./segmenter.onnx",
            "estimator": "./estimator.onnx",
            "boundaryToDuration": "./bd2dur.onnx",
            "timestep": NOTE_TIMESTEP,
            "languages": {"zxx": 0},
        }

    unnumbered = output / "fixture-note-unnumbered"
    write_package(
        unnumbered,
        "otter/fixture-note-unnumbered",
        {
            "note": {
                "interface": "org.openvpi.otter.analysis.Note",
                "level": 1,
                "variant": "game",
                "name": "Fixture Note promising a language it cannot number",
                "exports": {"sampleRate": NOTE_RATE, "languages": ["zxx", "eng"]},
                "configuration": note_models(unnumbered / "analyzers" / "note"),
            }
        },
    )

    no_alignment = output / "fixture-note-no-alignment"
    write_package(
        no_alignment,
        "otter/fixture-note-no-alignment",
        {
            "note": {
                "interface": "org.openvpi.otter.analysis.Note",
                "level": 1,
                "variant": "game",
                "name": "Fixture Note promising alignment without the model",
                "exports": {"sampleRate": NOTE_RATE, "supportsKnownNotes": True},
                "configuration": note_models(no_alignment / "analyzers" / "note"),
            }
        },
    )

    print(f"wrote {rmvpe}")
    print(f"wrote {note}")
    print(f"wrote {wrong_rate}, {unnumbered} and {no_alignment}, which must not load")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
