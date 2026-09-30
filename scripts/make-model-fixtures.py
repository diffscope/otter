#!/usr/bin/env python3
"""Builds analysis packages whose models have the real signatures and synthetic weights.

The tests of the shipped providers verify conformance to the contract: the correct tensors are
passed in, the outputs are read in the correct order, the knobs reach the model, and the returned
times are relative to the start time supplied by the host. These properties do not require a
trained model, and trained models are too large for the repository.

The fixtures are therefore real ONNX graphs with the real input and output names, shapes and
dtypes, computing arithmetic that a test can predict. A provider that swaps two inputs, drops a
knob or reads an output with the wrong dtype fails against the fixtures as it would against the
real weights.

The fixtures cannot verify the numerical accuracy of the results; that requires the trained
models.

Usage:

    python3 scripts/make-model-fixtures.py --output build/fixtures
"""

import argparse
import json
import shutil
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

# RMVPE runs at 16 kHz with a hop size of 160 samples, which is exactly 10 ms per frame.
RMVPE_RATE = 16000
RMVPE_HOP = 160

# The note model's config declares 44.1 kHz and a 10 ms frame.
NOTE_RATE = 44100
NOTE_TIMESTEP = 0.01

# The align model uses the same format, and its vocabulary assigns an index to each phoneme class.
# The fixture schedule uses the two words of the fixture dictionary and the separators around and
# between them. The class order below is the order in which the graph emits the classes and in
# which vocab.json declares them.
HFA_RATE = 44100
HFA_HOP = 441
HFA_CLASSES = ["SP", "zh/a", "zh/b", "AP", "EP", ""]

# The non-speech head has its own class indices, independent of the phoneme vocabulary: class zero
# is the background class that the reference implementation prepends to the non-lexical list. The
# two heads therefore assign different indices to "AP", and a fixture that emitted the non-speech
# classes in vocabulary order would feed the decoder the classes of the wrong head.
HFA_NON_SPEECH_CLASSES = ["None", "AP", "EP"]

# Schedule of the align graph, which the test reads from schedule.json instead of duplicating: the
# frames are divided into HFA_SPANS equal spans, and the breath starts HFA_BREATH_START_OFFSET frames
# after the start of the separator and ends HFA_BREATH_END_OFFSET frames before its end.
HFA_SPANS = 5
HFA_BREATH_START_OFFSET = 3
HFA_BREATH_END_OFFSET = 5

# Logit of the breath class; the logits of the other two non-speech classes are zero.
HFA_BREATH_LOGIT = 1.0


def _sorted(nodes: list, available: set) -> list:
    """Sorts nodes topologically so that every input is produced before a node reads it.

    ONNX requires a topologically sorted graph. Sorting here allows each builder to list its nodes
    in the order that best explains the computation, and reports an unresolvable input as an
    explicit error instead of a checker message about a single node.
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
            raise ValueError(f"no node produces {sorted(missing)}")
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

    The curve is a ramp so that a test can infer frame order from frame content, and the voicing
    flag is derived from the threshold so that the effect of the knob is observable. The flag
    follows the model's own convention, in which true marks an *unvoiced* frame; the provider
    inverts it before returning the result.
    """
    waveform = helper.make_tensor_value_info("waveform", TensorProto.FLOAT, [1, "samples"])
    threshold = helper.make_tensor_value_info("threshold", TensorProto.FLOAT, [])
    f0 = helper.make_tensor_value_info("f0", TensorProto.FLOAT, ["frames"])
    uv = helper.make_tensor_value_info("uv", TensorProto.BOOL, ["frames"])

    nodes = [
        # frames = samples // hop, computed from the shape of the waveform so that a provider
        # passing the wrong buffer produces an observably wrong length.
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
        # uv = (index % 100) >= round(threshold * 100): a higher threshold marks more frames as
        # unvoiced, so the effect of the knob is observable in the output.
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
        # The duration is folded into the features so that a provider passing a wrong duration
        # produces observably wrong features.
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
    """Builds a segmenter that places a boundary every N frames, with N derived from the radius.

    The real segmenter runs a diffusion loop; the fixture computes arithmetic over the same inputs.
    It preserves the properties that a provider can violate: every declared input must be supplied,
    the knobs must affect the result, and the shapes must match those of a real export.

    The last property determines that `t` has shape `[B]` instead of `["steps"]` and that the two
    knobs are scalars. An earlier version of the fixture declared the shapes that the provider
    sent, so the provider was verified against its own assumptions and passed. The same provider
    failed on the first call against a real GAME export: all segmenter inputs, `t` included, share
    one batch dimension, so each call carries one timestep and the sampling loop is the caller's
    responsibility.
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
        # period = max(radius, 10): a larger radius widens the notes. radius is a scalar, so it
        # is first reshaped into a one-element vector.
        _const("floor_p", np.array([10], dtype=np.int64)),
        helper.make_node("Unsqueeze", ["radius", "axis"], ["radius_1"]),
        helper.make_node("Max", ["radius_1", "floor_p"], ["period"]),
        helper.make_node("Mod", ["index", "period_b"], ["phase"]),
        helper.make_node("Reshape", ["period", "one"], ["period_b"]),
        _const("zero_b", np.array([0], dtype=np.int64)),
        helper.make_node("Equal", ["phase", "zero_b"], ["at_period"]),
        # A known boundary supplied by the caller is always kept, which exercises the alignment
        # path.
        helper.make_node("Or", ["at_period_1", "known_boundaries"], ["with_known"]),
        helper.make_node("Unsqueeze", ["at_period", "axis"], ["at_period_1"]),
        helper.make_node("And", ["with_known", "maskT"], ["boundaries"]),
        # Every remaining declared input is consumed, so the graph fails to run if a provider
        # omits one of them.
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

    Each note extends from one boundary to the next, so N is the number of boundaries and every
    duration equals the period in seconds.
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
        # Every note lasts one period, computed as T / N.
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

    This model implements the alignment path: known note durations are converted into the
    boundaries on which the segmenter is conditioned. A boundary is placed at the start of every
    note, computed as the cumulative sum of the preceding durations.
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
        # starts = exclusive cumulative sum of the durations, converted to frames.
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
        # A chromatic sequence starting at middle C, so that note order is observable in the
        # pitches.
        _const("middle_c", np.array([60.0], dtype=np.float32)),
        helper.make_node("Add", ["index_f", "middle_c"], ["scores_flat"]),
        helper.make_node("Unsqueeze", ["scores_flat", "axis"], ["scores"]),
        # presence alternates between present and absent so that a presence cutoff has an
        # observable effect. It is a boolean because the shipped model outputs a boolean presence
        # flag instead of a confidence; a host interprets it as a confidence of one or zero.
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


def build_hfa(path: Path, classes: list = HFA_CLASSES) -> None:
    """waveform [1, samples] -> ph_frame_logits [1, C, T], ph_edge_logits [1, T], cvnt_logits [1, C, T].

    The graph emits a fixed schedule instead of model predictions, and the test derives its
    expectations from that schedule. The frames are divided into five equal spans, one for each
    phoneme of the expansion of the lyrics "a b" by the dictionary. The edge head marks the four
    frame indices at which the spans meet, and the non-speech head marks a breath inside the middle
    separator. Every result of the decoder (the position of each word, the classification of each
    gap as an intra-word pause or as silence, and whether a breath exceeds the minimum reported
    duration) is therefore determined by the schedule, and the test can state the exact expected
    result.

    The frame count is computed from the shape of the waveform, so a provider passing the wrong
    buffer produces an observably wrong schedule instead of a plausible one.

    \a classes is the order in which the rows are emitted, which must equal the order declared by
    the paired vocab.json.
    """
    waveform = helper.make_tensor_value_info("waveform", TensorProto.FLOAT, [1, "samples"])
    frame_logits = helper.make_tensor_value_info(
        "ph_frame_logits", TensorProto.FLOAT, [1, len(classes), "frames"]
    )
    edge_logits = helper.make_tensor_value_info("ph_edge_logits", TensorProto.FLOAT, [1, "frames"])
    cvnt_logits = helper.make_tensor_value_info(
        "cvnt_logits", TensorProto.FLOAT, [1, len(HFA_NON_SPEECH_CLASSES), "frames"]
    )

    def row(condition: str, value: str) -> str:
        """Returns the row of one class: \a value in frames where \a condition holds, zero in
        all other frames."""
        name = f"row_{condition}"
        nodes.append(helper.make_node("Where", [condition, value, "zero_f"], [name]))
        return name

    nodes = [
        # frames = samples // hop
        helper.make_node("Shape", ["waveform"], ["shape"]),
        _const("one", np.array([1], dtype=np.int64)),
        _const("two_dim", np.array([2], dtype=np.int64)),
        helper.make_node("Slice", ["shape", "one", "two_dim"], ["samples"]),
        _const("hop", np.array([HFA_HOP], dtype=np.int64)),
        helper.make_node("Div", ["samples", "hop"], ["frames"]),
        # index = 0 .. frames-1
        _const("zero_i", np.array([0], dtype=np.int64)),
        _const("one_i", np.array([1], dtype=np.int64)),
        _const("zero_axis", np.array([0], dtype=np.int64)),
        helper.make_node("Squeeze", ["zero_i", "zero_axis"], ["zero_s"]),
        helper.make_node("Squeeze", ["frames", "zero_axis"], ["frames_s"]),
        helper.make_node("Squeeze", ["one_i", "zero_axis"], ["step_s"]),
        helper.make_node("Range", ["zero_s", "frames_s", "step_s"], ["index"]),
        # The spans meet at q, 2q, 3q and 4q, where q = frames // HFA_SPANS.
        _const("five_i", np.array([HFA_SPANS], dtype=np.int64)),
        _const("two_i", np.array([2], dtype=np.int64)),
        _const("three_i", np.array([3], dtype=np.int64)),
        _const("four_i", np.array([4], dtype=np.int64)),
        helper.make_node("Div", ["frames", "five_i"], ["q"]),
        helper.make_node("Mul", ["q", "two_i"], ["q2"]),
        helper.make_node("Mul", ["q", "three_i"], ["q3"]),
        helper.make_node("Mul", ["q", "four_i"], ["q4"]),
        helper.make_node("Less", ["index", "q"], ["before_q"]),
        helper.make_node("GreaterOrEqual", ["index", "q"], ["from_q"]),
        helper.make_node("Less", ["index", "q2"], ["before_q2"]),
        helper.make_node("GreaterOrEqual", ["index", "q2"], ["from_q2"]),
        helper.make_node("Less", ["index", "q3"], ["before_q3"]),
        helper.make_node("GreaterOrEqual", ["index", "q3"], ["from_q3"]),
        helper.make_node("Less", ["index", "q4"], ["before_q4"]),
        helper.make_node("GreaterOrEqual", ["index", "q4"], ["from_q4"]),
        # The four regions: the first word, the separator, the second word, the trailing silence.
        helper.make_node("And", ["from_q", "before_q2"], ["is_a"]),
        helper.make_node("And", ["from_q2", "before_q3"], ["is_separator"]),
        helper.make_node("And", ["from_q3", "before_q4"], ["is_b"]),
        helper.make_node("Or", ["before_q", "is_separator"], ["sp_head"]),
        helper.make_node("Or", ["sp_head", "from_q4"], ["is_sp"]),
        # The breath lies strictly inside the separator. The breath span must exceed the minimum
        # duration that the decoder reports; otherwise the fixture would test the duration
        # threshold instead of the schedule.
        _const("three_pad", np.array([HFA_BREATH_START_OFFSET], dtype=np.int64)),
        _const("five_pad", np.array([HFA_BREATH_END_OFFSET], dtype=np.int64)),
        helper.make_node("Add", ["q2", "three_pad"], ["breath_start"]),
        helper.make_node("Sub", ["q3", "five_pad"], ["breath_end"]),
        helper.make_node("GreaterOrEqual", ["index", "breath_start"], ["from_breath"]),
        helper.make_node("Less", ["index", "breath_end"], ["before_breath"]),
        helper.make_node("And", ["from_breath", "before_breath"], ["is_breath"]),
        # A frame assigned to a class carries a logit of ten and every other frame carries zero.
        # Only the relative order matters, and a margin of ten makes the argmax unambiguous.
        _const("high", np.array([10.0], dtype=np.float32)),
        _const("edge_high", np.array([10.0], dtype=np.float32)),
        _const("edge_low", np.array([-10.0], dtype=np.float32)),
        _const("zero_f", np.array([0.0], dtype=np.float32)),
        # The breath class has a probability of e / (e + 2) = 0.576 against the background and the
        # other non-speech class. The value lies above the default threshold of 0.5 and below 0.8,
        # so the tests can observe the effect of the threshold knob.
        _const("breath_logit", np.array([HFA_BREATH_LOGIT], dtype=np.float32)),
        helper.make_node("Cast", ["index"], ["index_f"], to=TensorProto.FLOAT),
        helper.make_node("Mul", ["index_f", "zero_f"], ["row_none"]),
        _const("silent_axis", np.array([0], dtype=np.int64)),
    ]
    nodes.extend(
        [
            helper.make_node("Equal", ["index", "q"], ["at_q"]),
            helper.make_node("Equal", ["index", "q2"], ["at_q2"]),
            helper.make_node("Equal", ["index", "q3"], ["at_q3"]),
            helper.make_node("Equal", ["index", "q4"], ["at_q4"]),
            helper.make_node(
                "Or", ["at_q", "at_q2"], ["edge_a"]
            ),
            helper.make_node(
                "Or", ["at_q3", "at_q4"], ["edge_b"]
            ),
            helper.make_node("Or", ["edge_a", "edge_b"], ["is_edge"]),
            helper.make_node("Where", ["is_edge", "edge_high", "edge_low"], ["edge_row"]),
            helper.make_node("Unsqueeze", ["edge_row", "silent_axis"], ["ph_edge_logits"]),
        ]
    )
    # The phoneme rows, one per class, in the class order declared by the vocabulary.
    scheduled = {"SP": row("is_sp", "high"), "zh/a": row("is_a", "high"),
                 "zh/b": row("is_b", "high")}
    frame_rows = [scheduled.get(name, "row_none") for name in classes]
    cvnt_rows = [
        "row_none",
        row("is_breath", "breath_logit"),
        "row_none",
    ]
    for rows, name in ((frame_rows, "frame_rows"), (cvnt_rows, "cvnt_rows")):
        for index, source in enumerate(rows):
            nodes.append(
                helper.make_node("Unsqueeze", [source, "silent_axis"], [f"{name}_{index}"])
            )

    nodes.extend(
        [
            helper.make_node("Concat", [f"frame_rows_{i}" for i in range(len(classes))],
                             ["frame_matrix"], axis=0),
            helper.make_node("Unsqueeze", ["frame_matrix", "silent_axis"], ["ph_frame_logits"]),
            helper.make_node("Concat", [f"cvnt_rows_{i}" for i in range(len(cvnt_rows))],
                             ["cvnt_matrix"], axis=0),
            helper.make_node("Unsqueeze", ["cvnt_matrix", "silent_axis"], ["cvnt_logits"]),
        ]
    )

    _save(
        helper.make_graph(
            nodes,
            "hfa",
            [waveform],
            [frame_logits, edge_logits, cvnt_logits],
        ),
        path,
    )


def write_hfa_model_files(
    directory: Path,
    *,
    rate: int = HFA_RATE,
    classes: list = HFA_CLASSES,
    dictionaries: dict = None,
    non_lexical: list = None,
    silent: list = None,
) -> dict:
    """Writes the model's own files, reduced to the classes that the fixture graph emits.

    The provider reads the sample rate and the hop size from config.json and the classes and
    dictionaries from vocab.json, and checks the declaration against both at load. Invented files
    would test the declaration reader against itself, so these files have the same keys, names
    and structure as the files of the released model; only the values differ.

    The keyword arguments serve the packages that must be rejected: each argument writes a value
    that contradicts the paired declaration. If the provider omits the corresponding check, the
    package loads and the test fails.

    :returns: the ``configuration`` block that refers to the written files.
    """
    config = {
        "mel_spec_config": {
            "sample_rate": rate,
            "hop_size": HFA_HOP,
            "num_mel_bins": 128,
        }
    }
    vocab = {
        "vocab": {name: index for index, name in enumerate(classes)},
        "vocab_size": len(classes),
        "silent_phonemes": ["", "SP", "AP", "EP"] if silent is None else silent,
        "non_lexical_phonemes": ["AP", "EP"] if non_lexical is None else non_lexical,
        "dictionaries": {"zh": "zh_dict.txt"} if dictionaries is None else dictionaries,
        "language_prefix": True,
    }
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "config.json").write_text(json.dumps(config, indent=4) + "\n", encoding="utf-8")
    (directory / "vocab.json").write_text(json.dumps(vocab, indent=4) + "\n", encoding="utf-8")
    # Two words of one phoneme each, so that the span of each word equals the span of its phoneme
    # and the test can derive every boundary from the lyrics alone.
    (directory / "zh_dict.txt").write_text("a\ta\nb\tb\n", encoding="utf-8")
    return {
        "model": "../../model.onnx",
        "config": "../../config.json",
        "vocab": "../../vocab.json",
        "languages": {"cmn": "zh"},
    }


def write_hfa_schedule(directory: Path) -> None:
    """Writes the schedule of the align graph to schedule.json.

    The test derives its expectations from the same values that the graph was built from instead
    of from a duplicate. The breath probability is the softmax of the breath logit against the
    other two non-speech classes, whose logits are zero.
    """
    probability = float(np.exp(HFA_BREATH_LOGIT) / (np.exp(HFA_BREATH_LOGIT) + 2.0))
    schedule = {
        "sampleRate": HFA_RATE,
        "hopSize": HFA_HOP,
        "spans": HFA_SPANS,
        "breathStartOffset": HFA_BREATH_START_OFFSET,
        "breathEndOffset": HFA_BREATH_END_OFFSET,
        "breathProbability": probability,
    }
    (directory / "schedule.json").write_text(json.dumps(schedule, indent=4) + "\n",
                                             encoding="utf-8")


def write_package(root: Path, identifier: str, contributions: dict) -> None:
    desc = {
        "$version": "1.0",
        "id": identifier,
        "version": "1.0.0.0",
        # A first release declares its own version as compatVersion, because no older version
        # exists. The field is declared instead of omitted so that the next release only updates
        # it.
        "compatVersion": "1.0.0.0",
        "runtimeLevel": 1,
        "contributions": {
            "inference": [
                {"id": name, "path": f"./inferences/{name}/inference.json"}
                for name in contributions
            ]
        },
    }
    (root / "desc.json").write_text(json.dumps(desc, indent=4) + "\n", encoding="utf-8")
    for name, declaration in contributions.items():
        directory = root / "inferences" / name
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "inference.json").write_text(
            json.dumps(declaration, indent=4) + "\n", encoding="utf-8"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", default="build/fixtures", type=Path,
                        help="output directory, replaced if it exists (default: build/fixtures)")
    args = parser.parse_args()

    output = args.output
    if output.exists():
        shutil.rmtree(output)

    rmvpe = output / "fixture-rmvpe"
    build_rmvpe(rmvpe / "inferences" / "f0" / "rmvpe.onnx")
    write_package(
        rmvpe,
        "otter/fixture-rmvpe",
        {
            "f0": {
                "interface": "org.openvpi.otter.inference.F0",
                "level": 1,
                "variant": "rmvpe",
                "name": "Fixture RMVPE",
                # The contract values that a host reads: the audio format and the knobs. The
                # interface defines the syntax, so the block has the same form for every variant.
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
                # The block that only the rmvpe variant reads: the path of its model.
                "configuration": {
                    "model": "./rmvpe.onnx",
                },
            }
        },
    )

    note = output / "fixture-note"
    models = note / "inferences" / "note"
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
                "interface": "org.openvpi.otter.inference.Note",
                "level": 1,
                "variant": "game",
                "name": "Fixture Note",
                "exports": {
                    "sampleRate": NOTE_RATE,
                    "channelCount": 1,
                    "maxSegmentDuration": 60.0,
                    "languages": ["zxx", "cmn"],
                    "defaultLanguage": "zxx",
                    "supportsKnownNotes": True,
                    "knobs": {
                        "boundaryThreshold": {"minimum": 0.0, "maximum": 1.0, "default": 0.2},
                        "boundaryRadius": {"minimum": 0.0, "maximum": 1.0, "default": 0.2},
                        "noteThreshold": {"minimum": 0.0, "maximum": 1.0, "default": 0.2},
                        "notePresenceCutoff": {"minimum": 0.0, "maximum": 1.0, "default": 0.5},
                        "steps": {"minimum": 1, "maximum": 1000, "default": 8},
                    },
                },
                # The block that only the game variant reads: its five models and their
                # parameters.
                "configuration": {
                    "encoder": "./encoder.onnx",
                    "segmenter": "./segmenter.onnx",
                    "estimator": "./estimator.onnx",
                    "boundaryToDuration": "./bd2dur.onnx",
                    "durationToBoundary": "./dur2bd.onnx",
                    "timestep": NOTE_TIMESTEP,
                    # eng is mapped but not declared: the model supports it and the package does
                    # not offer it, so an execution that requests it is rejected.
                    "languages": {"zxx": 0, "cmn": 1, "eng": 2},
                },
            }
        },
    )

    def align_declaration(exports: dict, configuration: dict) -> dict:
        """Returns the align declaration shared by all align fixtures, with \a exports merged
        into its exports."""
        declaration = {
            "interface": "org.openvpi.otter.inference.Align",
            "level": 1,
            "variant": "hfa",
            "name": "Fixture Align",
            "exports": {
                "sampleRate": HFA_RATE,
                "channelCount": 1,
                "maxSegmentDuration": 60.0,
                "languages": [
                    {"language": "cmn", "scheme": "pinyin", "lyrics": "scheme",
                     "phonemes": ["a", "b"]},
                ],
                "defaultLanguage": "cmn",
                "nonSpeechPhonemes": ["AP", "EP"],
                "defaultNonSpeechPhonemes": ["AP"],
                "silenceLabel": "SP",
                "knobs": {
                    "nonSpeechThreshold": {"minimum": 0.0, "maximum": 1.0, "default": 0.5},
                    "nonSpeechMinDuration": {"minimum": 0.0, "maximum": 2.0, "default": 0.1},
                    "gapFill": {"minimum": 0.0, "maximum": 1.0, "default": 0.1},
                },
            },
            "configuration": configuration,
        }
        declaration["exports"].update(exports)
        # A key with the value None is removed from the exports.
        declaration["exports"] = {key: value for key, value in declaration["exports"].items()
                                  if value is not None}
        return declaration

    align = output / "fixture-align"
    build_hfa(align / "model.onnx")
    write_package(
        align,
        "otter/fixture-align",
        {"align": align_declaration({}, write_hfa_model_files(align))},
    )
    write_hfa_schedule(align)

    # The same schedule with a different class order, in which the separator is not class zero.
    # The reference implementation assumes that it is; a provider with the same assumption would
    # decode "a" as the separator and place no word at the positions of the lyrics.
    reordered_classes = ["zh/a", "zh/b", "AP", "EP", "", "SP"]
    reordered = output / "fixture-align-reordered"
    build_hfa(reordered / "model.onnx", reordered_classes)
    write_package(
        reordered,
        "otter/fixture-align-reordered",
        {"align": align_declaration({}, write_hfa_model_files(reordered,
                                                              classes=reordered_classes))},
    )

    # The align packages that must not load. The provider reads the model's own files at load and
    # checks the declaration against them, so each package contains one value that contradicts its
    # declaration. If the provider omits the corresponding check, the package loads and the test
    # fails.
    align_rejects = []
    for name, exports, files in (
        ("wrong-rate", {}, {"rate": 16000}),
        ("no-dictionary", {}, {"dictionaries": {}}),
        ("unpromised-breath", {"nonSpeechPhonemes": ["AP", "EP"]}, {"non_lexical": ["AP"]}),
        ("unknown-silence", {}, {"silent": ["", "AP", "EP"]}),
        # A silence label that is not a class of the vocabulary cannot separate words.
        ("unclassed-silence", {"silenceLabel": "SIL"}, {"silent": ["", "SP", "AP", "EP", "SIL"]}),
        # The variant separates words with the silence label, so the label is required.
        ("no-silence", {"silenceLabel": None}, {}),
        ("wrong-phonemes",
         {"languages": [{"language": "cmn", "scheme": "pinyin", "lyrics": "scheme",
                         "phonemes": ["a", "c"]}]},
         {}),
    ):
        root = output / f"fixture-align-{name}"
        align_rejects.append(root)
        write_package(
            root,
            f"otter/fixture-align-{name}",
            {"align": align_declaration(exports, write_hfa_model_files(root, **files))},
        )

    # Declarations that the variants must reject at load: the contract syntax is valid, but the
    # declared values contradict the models. Each package has its own graphs, so the rejection
    # results from the declaration and not from a missing file.
    wrong_rate = output / "fixture-rmvpe-wrong-rate"
    build_rmvpe(wrong_rate / "inferences" / "f0" / "rmvpe.onnx")
    write_package(
        wrong_rate,
        "otter/fixture-rmvpe-wrong-rate",
        {
            "f0": {
                "interface": "org.openvpi.otter.inference.F0",
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
                "interface": "org.openvpi.otter.inference.Note",
                "level": 1,
                "variant": "game",
                "name": "Fixture Note promising a language it cannot number",
                "exports": {"sampleRate": NOTE_RATE, "languages": ["zxx", "eng"],
                            "defaultLanguage": "zxx"},
                "configuration": note_models(unnumbered / "inferences" / "note"),
            }
        },
    )

    no_alignment = output / "fixture-note-no-alignment"
    write_package(
        no_alignment,
        "otter/fixture-note-no-alignment",
        {
            "note": {
                "interface": "org.openvpi.otter.inference.Note",
                "level": 1,
                "variant": "game",
                "name": "Fixture Note promising alignment without the model",
                # The languages are declared so that the missing model is the only defect.
                "exports": {"sampleRate": NOTE_RATE, "languages": ["zxx"],
                            "defaultLanguage": "zxx", "supportsKnownNotes": True},
                "configuration": note_models(no_alignment / "inferences" / "note"),
            }
        },
    )

    no_default = output / "fixture-note-no-default"
    no_default_models = note_models(no_default / "inferences" / "note")
    write_package(
        no_default,
        "otter/fixture-note-no-default",
        {
            "note": {
                "interface": "org.openvpi.otter.inference.Note",
                "level": 1,
                "variant": "game",
                "name": "Fixture Note listing a language without naming a default",
                "exports": {"sampleRate": NOTE_RATE, "languages": ["zxx"]},
                "configuration": no_default_models,
            }
        },
    )

    print(f"wrote {rmvpe}")
    print(f"wrote {note}")
    print(f"wrote {align} and {reordered}")
    print(f"wrote {wrong_rate}, {unnumbered}, {no_alignment} and {no_default}, which must not load")
    print(f"wrote {', '.join(str(root) for root in align_rejects)}, which must not load")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
