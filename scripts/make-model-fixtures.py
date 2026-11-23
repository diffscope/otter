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

    python3 scripts/make-model-fixtures.py --output build/cmake/fixtures
"""

import argparse
import json
import shutil
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

# The reference evaluator is what runs the tifa variant's five graphs against the numpy reading of
# them.
from onnx.reference import ReferenceEvaluator

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

# Logit of the breath class. The logits of the other two non-speech classes are zero.
HFA_BREATH_LOGIT = 1.0

# The tifa variant declares the same frame length as the hfa one at a different rate, and its five
# graphs are the whole model, so the fixture has to carry all five. The two numbers the export bakes
# into fixed axes are the mel channels of its front end and the classes of its token head.
TIFA_RATE = 48000
TIFA_HOP = 480
TIFA_TIMESTEP = 0.01
TIFA_MELS = 80
TIFA_VOCAB = 256
TIFA_OPSET = 18

# What the token head puts on the class it was handed. Only the comparison between classes reaches
# a decoder, and a weight no other class can reach by accident makes a mixed up index visible.
TIFA_LOGIT_SCALE = 10.0

# The export numbers its symbols from three: zero, one and two are reserved for padding, masking and
# the word separator and are kept out of the symbol table, so no symbol id collides with a reserved
# one. The labels without a language prefix belong to no language in particular -- the export uses
# them for marks that stand outside the phoneme sets -- which is why a declaration's phoneme lists
# are read as the language-prefixed symbols alone.
TIFA_SYMBOLS = {
    "AP": 3,
    "EP": 4,
    "GS": 5,
    "zh/a": 6,
    "zh/b": 7,
    "zh/c": 8,
    "zh/d": 9,
    "zh/e": 10,
    "zh/f": 11,
    "zh/g": 12,
    "en/aa": 13,
    "en/ae": 14,
    "en/ah": 15,
    "en/ao": 16,
    "en/aw": 17,
    "en/ax": 18,
}

# Each language the fixture declares, as the host's identifier, the scheme the declaration names and
# the model's own code, which is what the export's vocabulary prefixes that language's symbols with.
TIFA_LANGUAGES = {
    "cmn": ("pinyin", "zh"),
    "eng": ("arpabet", "en"),
}

# One written unit per line, the phonemes it stands for after a tab. A dictionary maps the spelling
# a host is given to the model's symbols, and the fixture's entries are its own, so a test can name
# an entry and know which phonemes it has to produce.
TIFA_DICTIONARY_ENTRIES = {
    "cmn": ["a\ta", "ba\tb a", "ce\tc e", "da\td a", "fe\tf e", "g\tg"],
    "eng": ["aa\taa", "ae\tae", "ah\tah", "ao\tao", "aw\taw", "ax\tax"],
}


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


def _save(graph: onnx.GraphProto, path: Path, opset: int = 17) -> None:
    available = {value.name for value in graph.input}
    available.update(value.name for value in graph.initializer)
    ordered = _sorted(list(graph.node), available)
    del graph.node[:]
    graph.node.extend(ordered)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", opset)])
    model.ir_version = 10
    onnx.checker.check_model(model)
    path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, str(path))


def _const(name: str, array: np.ndarray) -> onnx.NodeProto:
    return helper.make_node(
        "Constant", [], [name], value=numpy_helper.from_array(array, name + "_value")
    )


def _dim(nodes: list, source: str, axis: int, name: str) -> str:
    """Appends the nodes that read shape(\a source)[\a axis] and returns the scalar's name.

    Every one of the aligner's graphs is dynamic in its batch and slot axes and reads those axes back
    off its own inputs rather than assuming the sizes one call happens to have. A fixture has to do
    the same: a graph that declared its batch fixed would be checking the caller against the
    fixture's assumption instead of against the model.
    """
    nodes.append(helper.make_node("Shape", [source], [f"{name}_shape"]))
    nodes.append(_const(f"{name}_start", np.array([axis], dtype=np.int64)))
    nodes.append(_const(f"{name}_stop", np.array([axis + 1], dtype=np.int64)))
    nodes.append(
        helper.make_node("Slice", [f"{name}_shape", f"{name}_start", f"{name}_stop"], [f"{name}_of"])
    )
    nodes.append(_const(f"{name}_slice_axis", np.array([0], dtype=np.int64)))
    nodes.append(helper.make_node("Squeeze", [f"{name}_of", f"{name}_slice_axis"], [name]))
    return name


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
        # flag instead of a confidence. A host interprets it as a confidence of one or zero.
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
        # duration that the decoder reports. Otherwise the fixture would test the duration
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


def build_tifa_spectrogram(path: Path) -> None:
    """waveform [B, L] + duration [B] -> spectrogram [B, T, 80] float, maskT [B, T] bool.

    The real front end is a windowed mel spectrogram; what a host can get wrong about it is the
    frame grid and the mask, so the fixture keeps both and replaces the transform with the mean of
    each hop, written into every mel channel. The frame count is read from the waveform's own length,
    so a provider handing over the wrong buffer produces a visibly wrong grid rather than a plausible
    one, and the mask is cut with Round, whose ties go to the even integer exactly as torch.round
    does: a host that rounds the other way disagrees on the boundary frame and nowhere else, which
    makes the convention worth pinning down here rather than in a test far from this rule.
    """
    waveform = helper.make_tensor_value_info("waveform", TensorProto.FLOAT, ["B", "L"])
    duration = helper.make_tensor_value_info("duration", TensorProto.FLOAT, ["B"])
    spectrogram = helper.make_tensor_value_info(
        "spectrogram", TensorProto.FLOAT, ["B", "T", TIFA_MELS]
    )
    maskT = helper.make_tensor_value_info("maskT", TensorProto.BOOL, ["B", "T"])

    nodes = []
    batch = _dim(nodes, "waveform", 0, "batch")
    length = _dim(nodes, "waveform", 1, "length")
    nodes.append(_const("axis_0", np.array([0], dtype=np.int64)))
    nodes.append(_const("axis_1", np.array([1], dtype=np.int64)))
    nodes.append(_const("axis_2", np.array([2], dtype=np.int64)))
    nodes.append(_const("hop", np.array(TIFA_HOP, dtype=np.int64)))
    nodes.append(helper.make_node("Div", [length, "hop"], ["T"]))
    nodes.append(helper.make_node("Unsqueeze", [batch, "axis_0"], ["batch_1"]))
    nodes.append(helper.make_node("Unsqueeze", ["T", "axis_0"], ["T_1"]))
    # A length that stops mid hop leaves a tail the real front end never reads, so the samples are
    # cut to whole hops before they are shaped into the frames of the answer.
    nodes.append(helper.make_node("Mul", ["T_1", "hop"], ["window"]))
    nodes.append(_const("start", np.array([0], dtype=np.int64)))
    nodes.append(helper.make_node("Slice", ["waveform", "start", "window", "axis_1"], ["usable"]))
    nodes.append(_const("hop_1", np.array([TIFA_HOP], dtype=np.int64)))
    nodes.append(helper.make_node("Concat", ["batch_1", "T_1", "hop_1"], ["hop_shape"], axis=0))
    nodes.append(helper.make_node("Reshape", ["usable", "hop_shape"], ["hops"]))
    nodes.append(helper.make_node("ReduceMean", ["hops", "axis_2"], ["hop_mean"], keepdims=0))
    nodes.append(helper.make_node("Unsqueeze", ["hop_mean", "axis_2"], ["hop_frames"]))
    nodes.append(_const("mels", np.array([TIFA_MELS], dtype=np.int64)))
    nodes.append(helper.make_node("Concat", ["batch_1", "T_1", "mels"], ["mel_shape"], axis=0))
    nodes.append(helper.make_node("Expand", ["hop_frames", "mel_shape"], ["spectrogram"]))
    # maskT = t < round(duration / timestep), the frames the host said the audio occupies.
    nodes.append(_const("timestep", np.array(TIFA_TIMESTEP, dtype=np.float32)))
    nodes.append(helper.make_node("Div", ["duration", "timestep"], ["wanted_f"]))
    nodes.append(helper.make_node("Round", ["wanted_f"], ["wanted_r"]))
    nodes.append(helper.make_node("Cast", ["wanted_r"], ["wanted"], to=TensorProto.INT64))
    nodes.append(helper.make_node("Unsqueeze", ["wanted", "axis_1"], ["wanted_column"]))
    nodes.append(_const("zero", np.array(0, dtype=np.int64)))
    nodes.append(_const("one", np.array(1, dtype=np.int64)))
    nodes.append(helper.make_node("Range", ["zero", "T", "one"], ["index"]))
    nodes.append(helper.make_node("Unsqueeze", ["index", "axis_0"], ["index_row"]))
    nodes.append(helper.make_node("Less", ["index_row", "wanted_column"], ["maskT"]))
    _save(
        helper.make_graph(
            nodes, "tifa_spectrogram", [waveform, duration], [spectrogram, maskT]
        ),
        path,
        opset=TIFA_OPSET,
    )


def build_tifa_model(path: Path) -> None:
    """spectrogram [B, T, 80] + tokens [B, N] -> similarities [B, T, N] float, logits [B, N, 256].

    The similarity is a ramp that peaks where a frame's share of the audio meets a token's share of
    the tokens, so a decoder reading it places spans in proportion to how many tokens there are and
    a host that mixed the two axes up produces spans running the wrong way. The logits put all of
    their weight on the token the host handed over, which turns a wrong class or a wrong vocabulary
    size into a visibly wrong one-hot instead of a plausible distribution.

    maskT and maskN are declared because the real model takes them and a call has to carry them; this
    arithmetic has nothing to hide, so it reads neither. That is also the one thing a host can get
    wrong with them: a call that leaves one out does not run at all.
    """
    spectrogram = helper.make_tensor_value_info(
        "spectrogram", TensorProto.FLOAT, ["B", "T", TIFA_MELS]
    )
    tokens = helper.make_tensor_value_info("tokens", TensorProto.INT64, ["B", "N"])
    maskT = helper.make_tensor_value_info("maskT", TensorProto.BOOL, ["B", "T"])
    maskN = helper.make_tensor_value_info("maskN", TensorProto.BOOL, ["B", "N"])
    similarities = helper.make_tensor_value_info(
        "similarities", TensorProto.FLOAT, ["B", "T", "N"]
    )
    logits = helper.make_tensor_value_info("logits", TensorProto.FLOAT, ["B", "N", TIFA_VOCAB])

    nodes = []
    batch = _dim(nodes, "spectrogram", 0, "batch")
    frames = _dim(nodes, "spectrogram", 1, "frames")
    count = _dim(nodes, "tokens", 1, "count")
    nodes.append(_const("axis_0", np.array([0], dtype=np.int64)))
    nodes.append(_const("axis_1", np.array([1], dtype=np.int64)))
    nodes.append(_const("zero", np.array(0, dtype=np.int64)))
    nodes.append(_const("one", np.array(1, dtype=np.int64)))
    nodes.append(helper.make_node("Range", ["zero", "frames", "one"], ["frame_index"]))
    nodes.append(helper.make_node("Range", ["zero", "count", "one"], ["token_index"]))
    nodes.append(helper.make_node("Cast", ["frame_index"], ["frame_f"], to=TensorProto.FLOAT))
    nodes.append(helper.make_node("Cast", ["frames"], ["frames_f"], to=TensorProto.FLOAT))
    nodes.append(helper.make_node("Div", ["frame_f", "frames_f"], ["frame_share"]))
    nodes.append(helper.make_node("Cast", ["token_index"], ["token_f"], to=TensorProto.FLOAT))
    nodes.append(helper.make_node("Cast", ["count"], ["count_f"], to=TensorProto.FLOAT))
    nodes.append(helper.make_node("Div", ["token_f", "count_f"], ["token_share"]))
    # similarities[b, t, n] = 1 - |t / T - n / N|
    nodes.append(helper.make_node("Unsqueeze", ["frame_share", "axis_1"], ["frame_column"]))
    nodes.append(helper.make_node("Unsqueeze", ["token_share", "axis_0"], ["token_row"]))
    nodes.append(helper.make_node("Sub", ["frame_column", "token_row"], ["gap"]))
    nodes.append(helper.make_node("Abs", ["gap"], ["distance"]))
    nodes.append(_const("one_f", np.array(1.0, dtype=np.float32)))
    nodes.append(helper.make_node("Sub", ["one_f", "distance"], ["ramp"]))
    # One batch axis of one, so the expansion below only ever copies the ramp across a batch: a
    # broadcast that found the frame count in that position would grow the frames instead.
    nodes.append(helper.make_node("Unsqueeze", ["ramp", "axis_0"], ["ramp_row"]))
    nodes.append(helper.make_node("Unsqueeze", [batch, "axis_0"], ["batch_1"]))
    nodes.append(helper.make_node("Unsqueeze", ["frames", "axis_0"], ["frames_1"]))
    nodes.append(helper.make_node("Unsqueeze", ["count", "axis_0"], ["count_1"]))
    nodes.append(
        helper.make_node(
            "Concat", ["batch_1", "frames_1", "count_1"], ["similarity_shape"], axis=0
        )
    )
    nodes.append(helper.make_node("Expand", ["ramp_row", "similarity_shape"], ["similarities"]))
    # logits[b, n, :] = one_hot(tokens[b, n], 256) * TIFA_LOGIT_SCALE, so the answer is the token
    # the host asked about rather than anything the graph decided.
    nodes.append(_const("classes", np.array(TIFA_VOCAB, dtype=np.int64)))
    nodes.append(_const("weights", np.array([0.0, TIFA_LOGIT_SCALE], dtype=np.float32)))
    nodes.append(helper.make_node("OneHot", ["tokens", "classes", "weights"], ["logits"], axis=-1))
    _save(
        helper.make_graph(
            nodes,
            "tifa_model",
            [spectrogram, tokens, maskT, maskN],
            [similarities, logits],
        ),
        path,
        opset=TIFA_OPSET,
    )


def build_tifa_prepare(path: Path) -> None:
    """paths [B, P, C] + words [B, P] -> tokens, segments, mapping [B, P] int64.

    The grid holds one row per slot of the template and one column per pronunciation candidate. A
    row whose candidates fill the same slots with the same tokens offers the host no choice, so it
    differs from nothing: `mapping` numbers the rows that do differ, `segments` groups the runs of
    consecutive differing rows that share a word -- the slots one candidate moves as a unit -- and
    `tokens` is the first candidate's reading of every row, which is the template the model is asked
    about before any candidate has been chosen.

    candidates and grouped are declared because the real graph takes them and a call has to carry
    them; the template this shortens to is built from the grid, so it reads neither.
    """
    paths = helper.make_tensor_value_info("paths", TensorProto.INT64, ["B", "P", "C"])
    words = helper.make_tensor_value_info("words", TensorProto.INT64, ["B", "P"])
    candidates = helper.make_tensor_value_info("candidates", TensorProto.BOOL, ["B", "W", "C"])
    grouped = helper.make_tensor_value_info("grouped", TensorProto.BOOL, [])
    tokens = helper.make_tensor_value_info("tokens", TensorProto.INT64, ["B", "P"])
    segments = helper.make_tensor_value_info("segments", TensorProto.INT64, ["B", "P"])
    mapping = helper.make_tensor_value_info("mapping", TensorProto.INT64, ["B", "P"])

    nodes = []
    batch = _dim(nodes, "paths", 0, "batch")
    nodes.append(_const("axis_0", np.array([0], dtype=np.int64)))
    nodes.append(_const("axis_1", np.array([1], dtype=np.int64)))
    nodes.append(_const("axis_2", np.array([2], dtype=np.int64)))
    # CumSum takes its axis as a scalar input tensor, unlike the axes of Slice and the reductions,
    # which are one-dimensional. A rank-1 axis is refused by the onnx reference evaluator, so the
    # two CumSum nodes below read this one and not axis_1.
    nodes.append(_const("cum_axis", np.array(1, dtype=np.int64)))
    nodes.append(_const("zero", np.array(0, dtype=np.int64)))
    # A row differs when the slots it fills do not all hold the same token. A row that fills none
    # agrees with itself, which keeps a padding row out of every fragment.
    nodes.append(helper.make_node("Equal", ["paths", "zero"], ["is_empty"]))
    nodes.append(helper.make_node("Not", ["is_empty"], ["filled"]))
    nodes.append(helper.make_node("Cast", ["filled"], ["filled_i"], to=TensorProto.INT64))
    nodes.append(
        helper.make_node("ReduceSum", ["filled_i", "axis_2"], ["filled_count"], keepdims=0)
    )
    nodes.append(_const("beyond", np.array(np.iinfo(np.int64).max, dtype=np.int64)))
    nodes.append(helper.make_node("Where", ["filled", "paths", "beyond"], ["filled_low"]))
    nodes.append(helper.make_node("ReduceMin", ["filled_low", "axis_2"], ["lowest"], keepdims=0))
    nodes.append(helper.make_node("Where", ["filled", "paths", "zero"], ["filled_high"]))
    nodes.append(helper.make_node("ReduceMax", ["filled_high", "axis_2"], ["highest"], keepdims=0))
    nodes.append(helper.make_node("Greater", ["filled_count", "zero"], ["has_slot"]))
    nodes.append(helper.make_node("Equal", ["lowest", "highest"], ["agrees"]))
    nodes.append(helper.make_node("Not", ["agrees"], ["disagrees"]))
    nodes.append(helper.make_node("And", ["has_slot", "disagrees"], ["differs"]))
    # mapping[row] = the 1-based number of the row among the differing rows, zero elsewhere.
    nodes.append(helper.make_node("Cast", ["differs"], ["differs_i"], to=TensorProto.INT64))
    nodes.append(helper.make_node("CumSum", ["differs_i", "cum_axis"], ["ordinal"]))
    nodes.append(helper.make_node("Where", ["differs", "ordinal", "zero"], ["mapping"]))
    # A segment is a run of consecutive differing rows that share a word. A row that does not differ
    # ends the run even when its word is the same one.
    nodes.append(_const("first", np.array([0], dtype=np.int64)))
    nodes.append(_const("last", np.array([-1], dtype=np.int64)))
    nodes.append(
        helper.make_node("Slice", ["differs", "first", "last", "axis_1"], ["earlier_differs"])
    )
    nodes.append(
        helper.make_node("Slice", ["words", "first", "last", "axis_1"], ["earlier_words"])
    )
    nodes.append(helper.make_node("Unsqueeze", [batch, "axis_0"], ["batch_1"]))
    nodes.append(_const("one_1", np.array([1], dtype=np.int64)))
    nodes.append(helper.make_node("Concat", ["batch_1", "one_1"], ["column_shape"], axis=0))
    nodes.append(_const("false", np.array(False, dtype=bool)))
    nodes.append(helper.make_node("Expand", ["false", "column_shape"], ["false_column"]))
    nodes.append(_const("zero_scalar", np.array(0, dtype=np.int64)))
    nodes.append(helper.make_node("Expand", ["zero_scalar", "column_shape"], ["zero_column"]))
    nodes.append(
        helper.make_node(
            "Concat", ["false_column", "earlier_differs"], ["previous_differs"], axis=1
        )
    )
    nodes.append(
        helper.make_node("Concat", ["zero_column", "earlier_words"], ["previous_words"], axis=1)
    )
    nodes.append(helper.make_node("Equal", ["previous_words", "words"], ["same_word"]))
    nodes.append(helper.make_node("And", ["previous_differs", "same_word"], ["continues"]))
    nodes.append(helper.make_node("Not", ["continues"], ["breaks"]))
    nodes.append(helper.make_node("And", ["differs", "breaks"], ["starts_segment"]))
    nodes.append(helper.make_node("Cast", ["starts_segment"], ["starts_i"], to=TensorProto.INT64))
    nodes.append(helper.make_node("CumSum", ["starts_i", "cum_axis"], ["segment_ordinal"]))
    nodes.append(helper.make_node("Where", ["differs", "segment_ordinal", "zero"], ["segments"]))
    # tokens = the first candidate column, the row's own reading of its slot.
    nodes.append(helper.make_node("Slice", ["paths", "first", "one_1", "axis_2"], ["first_column"]))
    nodes.append(helper.make_node("Squeeze", ["first_column", "axis_2"], ["tokens"]))
    _save(
        helper.make_graph(
            nodes,
            "tifa_prepare",
            [paths, words, candidates, grouped],
            [tokens, segments, mapping],
        ),
        path,
        opset=TIFA_OPSET,
    )


def build_tifa_score(path: Path) -> None:
    """logits + paths + words + segments + mapping -> the fragment table a host's DP reads.

    A fragment is one value of `mapping`: the rows the host decided share a choice. Its descriptor is
    the largest word and the largest segment inside it, `lengths` counts the rows of the fragment
    that fill each candidate's slots, and `capacity` counts the rows of every segment, which is what
    a search over slot assignments needs to know about a segment before it walks it.

    The costs and the tails are zero on purpose. The fixture has no opinion about which pronunciation
    is better, so it leaves the choice to the host's own order instead of encoding an algorithm in
    numbers a test would then be asserting. logits arrives because the host has it by then and a call
    carries it; the cost written here does not read it.
    """
    logits = helper.make_tensor_value_info("logits", TensorProto.FLOAT, ["B", "P", TIFA_VOCAB])
    paths = helper.make_tensor_value_info("paths", TensorProto.INT64, ["B", "P", "C"])
    words = helper.make_tensor_value_info("words", TensorProto.INT64, ["B", "P"])
    segments = helper.make_tensor_value_info("segments", TensorProto.INT64, ["B", "P"])
    mapping = helper.make_tensor_value_info("mapping", TensorProto.INT64, ["B", "P"])
    descriptors = helper.make_tensor_value_info("descriptors", TensorProto.INT64, ["B", "P1", 2])
    lengths = helper.make_tensor_value_info("lengths", TensorProto.INT64, ["B", "P1", "C"])
    costs = helper.make_tensor_value_info("costs", TensorProto.FLOAT, ["B", "P1", "C", "P1"])
    tails = helper.make_tensor_value_info("tails", TensorProto.FLOAT, ["B", "P1", "P1"])
    capacity = helper.make_tensor_value_info("capacity", TensorProto.INT64, ["B", "P1"])

    nodes = []
    batch = _dim(nodes, "paths", 0, "batch")
    columns = _dim(nodes, "paths", 2, "columns")
    nodes.append(_const("axis_0", np.array([0], dtype=np.int64)))
    nodes.append(_const("axis_1", np.array([1], dtype=np.int64)))
    nodes.append(_const("axis_2", np.array([2], dtype=np.int64)))
    nodes.append(_const("axes_01", np.array([0, 1], dtype=np.int64)))
    nodes.append(_const("flat", np.array([-1], dtype=np.int64)))
    # The fragments are the values the host put in mapping, whatever they are: the fixture never
    # assumes how it numbered its choices, only that equal numbers mean the same one.
    nodes.append(helper.make_node("Reshape", ["mapping", "flat"], ["mapping_flat"]))
    nodes.append(helper.make_node("Unique", ["mapping_flat"], ["fragments"], sorted=1))
    fragment_count = _dim(nodes, "fragments", 0, "fragment_count")
    nodes.append(helper.make_node("Unsqueeze", ["mapping", "axis_2"], ["mapping_column"]))
    nodes.append(helper.make_node("Unsqueeze", ["fragments", "axes_01"], ["fragment_row"]))
    nodes.append(helper.make_node("Equal", ["mapping_column", "fragment_row"], ["member"]))
    nodes.append(helper.make_node("Cast", ["member"], ["member_i"], to=TensorProto.INT64))
    nodes.append(helper.make_node("Cast", ["member"], ["member_f"], to=TensorProto.FLOAT))
    # descriptors[f] = (the largest word in the fragment, the largest segment in it).
    nodes.append(helper.make_node("Unsqueeze", ["words", "axis_2"], ["words_column"]))
    nodes.append(helper.make_node("Mul", ["words_column", "member_i"], ["words_membered"]))
    nodes.append(
        helper.make_node("ReduceMax", ["words_membered", "axis_1"], ["word_max"], keepdims=0)
    )
    nodes.append(helper.make_node("Unsqueeze", ["segments", "axis_2"], ["segments_column"]))
    nodes.append(helper.make_node("Mul", ["segments_column", "member_i"], ["segments_membered"]))
    nodes.append(
        helper.make_node("ReduceMax", ["segments_membered", "axis_1"], ["segment_max"], keepdims=0)
    )
    nodes.append(helper.make_node("Unsqueeze", ["word_max", "axis_2"], ["word_max_1"]))
    nodes.append(helper.make_node("Unsqueeze", ["segment_max", "axis_2"], ["segment_max_1"]))
    nodes.append(helper.make_node("Concat", ["word_max_1", "segment_max_1"], ["descriptors"], axis=2))
    # lengths[f][c] = how many rows of fragment f fill a slot of candidate c, which is the fragment's
    # membership matrix times the grid's own record of which slots are filled.
    nodes.append(_const("zero", np.array(0, dtype=np.int64)))
    nodes.append(helper.make_node("Equal", ["paths", "zero"], ["is_empty"]))
    nodes.append(helper.make_node("Not", ["is_empty"], ["filled"]))
    nodes.append(helper.make_node("Cast", ["filled"], ["filled_f"], to=TensorProto.FLOAT))
    nodes.append(helper.make_node("Transpose", ["member_f"], ["member_t"], perm=[0, 2, 1]))
    nodes.append(helper.make_node("MatMul", ["member_t", "filled_f"], ["lengths_f"]))
    nodes.append(helper.make_node("Cast", ["lengths_f"], ["lengths"], to=TensorProto.INT64))
    # capacity[s] = how many rows carry segment s.
    nodes.append(_const("one", np.array(1, dtype=np.int64)))
    nodes.append(helper.make_node("Add", [fragment_count, "one"], ["segment_limit"]))
    nodes.append(helper.make_node("Range", ["one", "segment_limit", "one"], ["segment_ids"]))
    nodes.append(helper.make_node("Unsqueeze", ["segment_ids", "axes_01"], ["segment_id_row"]))
    nodes.append(helper.make_node("Unsqueeze", ["segments", "axis_2"], ["segments_here"]))
    nodes.append(helper.make_node("Equal", ["segments_here", "segment_id_row"], ["in_segment"]))
    nodes.append(helper.make_node("Cast", ["in_segment"], ["in_segment_f"], to=TensorProto.FLOAT))
    nodes.append(
        helper.make_node("ReduceSum", ["in_segment_f", "axis_1"], ["capacity_f"], keepdims=0)
    )
    nodes.append(helper.make_node("Cast", ["capacity_f"], ["capacity"], to=TensorProto.INT64))
    # Zero costs: the host's own order breaks the tie between candidates.
    nodes.append(_const("zero_f", np.array(0.0, dtype=np.float32)))
    nodes.append(helper.make_node("Unsqueeze", [batch, "axis_0"], ["batch_1"]))
    nodes.append(helper.make_node("Unsqueeze", [fragment_count, "axis_0"], ["fragment_1"]))
    nodes.append(helper.make_node("Unsqueeze", [columns, "axis_0"], ["columns_1"]))
    nodes.append(
        helper.make_node(
            "Concat", ["batch_1", "fragment_1", "columns_1", "fragment_1"], ["cost_shape"], axis=0
        )
    )
    nodes.append(helper.make_node("Expand", ["zero_f", "cost_shape"], ["costs"]))
    nodes.append(
        helper.make_node(
            "Concat", ["batch_1", "fragment_1", "fragment_1"], ["tail_shape"], axis=0
        )
    )
    nodes.append(helper.make_node("Expand", ["zero_f", "tail_shape"], ["tails"]))
    _save(
        helper.make_graph(
            nodes,
            "tifa_score",
            [logits, paths, words, segments, mapping],
            [descriptors, lengths, costs, tails, capacity],
        ),
        path,
        opset=TIFA_OPSET,
    )


def build_tifa_select(path: Path) -> None:
    """paths + words + groups + choices -> best_tokens, best_words, best_groups, maskN [B, P].

    `choices[w]` is one based, and that is not a detail of the fixture: it is what the export reads
    and what the host writes, so a zero based fixture would answer the host with another word's
    column -- or, past the table's end, with nothing at all. Zero means the word was not read; a row
    whose chosen column holds no token is empty for another reason and takes the same answers -- a
    zero token, no word and a cleared mask -- because either way there is no phoneme there for the
    decode to place.

    A host filters the alignment's slots with that mask rather than with the zero it also finds in
    `best_tokens`, because a zero token is what a chosen column may legitimately hold.

    The one place this graph is simpler than the export is the order of its rows: the export sorts
    the rows that carry no token to the end of the tensor, and the host reads the result row by row
    against the grid it handed over. Its grids keep every unread row last already, so the sort is the
    identity on every input the host can produce, and leaving it out keeps row p meaning row p.
    """
    paths = helper.make_tensor_value_info("paths", TensorProto.INT64, ["B", "P", "C"])
    words = helper.make_tensor_value_info("words", TensorProto.INT64, ["B", "P"])
    groups = helper.make_tensor_value_info("groups", TensorProto.INT64, ["B", "P", "C"])
    choices = helper.make_tensor_value_info("choices", TensorProto.INT64, ["B", "W"])
    best_tokens = helper.make_tensor_value_info("best_tokens", TensorProto.INT64, ["B", "P"])
    best_words = helper.make_tensor_value_info("best_words", TensorProto.INT64, ["B", "P"])
    best_groups = helper.make_tensor_value_info("best_groups", TensorProto.INT64, ["B", "P"])
    maskN = helper.make_tensor_value_info("maskN", TensorProto.BOOL, ["B", "P"])

    nodes = []
    word_count = _dim(nodes, "choices", 1, "word_count")
    column_count = _dim(nodes, "paths", 2, "column_count")
    nodes.append(_const("axis_2", np.array([2], dtype=np.int64)))
    nodes.append(_const("zero", np.array(0, dtype=np.int64)))
    nodes.append(_const("one", np.array(1, dtype=np.int64)))
    nodes.append(_const("negative_one", np.array(-1, dtype=np.int64)))
    # words is 1-based and zero marks a padding row, so the lookup index is words - 1 clamped into
    # the table: a padding row reads no word's choice, and gathering one from the end would be an
    # index out of a promise rather than out of a bound.
    nodes.append(helper.make_node("Sub", ["words", "one"], ["word_index"]))
    nodes.append(helper.make_node("Less", ["word_index", "zero"], ["before_first"]))
    nodes.append(helper.make_node("Where", ["before_first", "zero", "word_index"], ["from_zero"]))
    nodes.append(helper.make_node("Sub", [word_count, "one"], ["last_word"]))
    nodes.append(helper.make_node("Greater", ["from_zero", "last_word"], ["past_last"]))
    nodes.append(helper.make_node("Where", ["past_last", "last_word", "from_zero"], ["word_column"]))
    # One chosen column per grid row, which is a lookup row by row rather than a table gather: Gather
    # would splice the index tensor's own axes into the result and hand back a batch axis twice.
    nodes.append(helper.make_node("GatherElements", ["choices", "word_column"], ["choice"], axis=1))
    nodes.append(helper.make_node("Equal", ["choice", "zero"], ["unread"]))
    nodes.append(helper.make_node("Equal", ["words", "zero"], ["padding"]))
    nodes.append(helper.make_node("Or", ["unread", "padding"], ["no_word"]))
    # The choice numbers the columns from one and zero already means "no column", so the token's own
    # index is the choice minus one. An unread row's index is pulled back into the table for the same
    # reason as above and then thrown away by the mask rather than read from a neighbour.
    nodes.append(helper.make_node("Sub", ["choice", "one"], ["chosen_column"]))
    nodes.append(helper.make_node("Less", ["chosen_column", "zero"], ["before_table"]))
    nodes.append(helper.make_node("Where", ["before_table", "zero", "chosen_column"], ["in_table"]))
    nodes.append(helper.make_node("Add", [column_count, "negative_one"], ["final_column"]))
    nodes.append(helper.make_node("Greater", ["in_table", "final_column"], ["past_table"]))
    nodes.append(
        helper.make_node("Where", ["past_table", "final_column", "in_table"], ["token_column"])
    )
    nodes.append(helper.make_node("Unsqueeze", ["token_column", "axis_2"], ["token_index"]))
    nodes.append(helper.make_node("GatherElements", ["paths", "token_index"], ["row"], axis=2))
    nodes.append(helper.make_node("Squeeze", ["row", "axis_2"], ["row_token"]))
    nodes.append(helper.make_node("GatherElements", ["groups", "token_index"], ["row_group"], axis=2))
    nodes.append(helper.make_node("Squeeze", ["row_group", "axis_2"], ["row_of_group"]))
    nodes.append(helper.make_node("Where", ["no_word", "zero", "row_token"], ["best_tokens"]))
    nodes.append(helper.make_node("Equal", ["best_tokens", "zero"], ["no_token"]))
    nodes.append(helper.make_node("Not", ["no_token"], ["maskN"]))
    nodes.append(helper.make_node("Where", ["maskN", "words", "zero"], ["best_words"]))
    nodes.append(helper.make_node("Where", ["maskN", "row_of_group", "zero"], ["best_groups"]))
    _save(
        helper.make_graph(
            nodes,
            "tifa_select",
            [paths, words, groups, choices],
            [best_tokens, best_words, best_groups, maskN],
        ),
        path,
        opset=TIFA_OPSET,
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


def write_tifa_model_files(
    directory: Path,
    *,
    rate: int = TIFA_RATE,
    dictionaries: dict = None,
    absent: tuple = (),
) -> dict:
    """The files the real export writes beside its graphs, cut down to the fixture's symbols.

    A provider reads the audio timing out of config.json and the symbols out of vocabulary.json, and
    checks the declaration against both at load, so these carry the export's own keys rather than
    names this script found convenient. The dictionaries are the declaration's own business -- the
    export does not name them -- so one key per declared language is written here, for the same
    reason the hfa fixture's vocabulary has to name one per language.

    \a dictionaries maps a language to the file its key points at; a language left out of the mapping
    gets no key at all. \a absent names languages whose file is deliberately not written, so the
    declaration points at a file that is not there. Both are what the packages that must be refused
    are made of: each carries one fact the declaration it is paired with claims otherwise.

    :returns: the ``configuration`` block that points at what was written.
    """
    files = (
        {
            "cmn": "dictionaries/fixture-cmn.txt",
            "eng": "dictionaries/fixture-eng.txt",
        }
        if dictionaries is None
        else dict(dictionaries)
    )
    config = {
        "samplerate": rate,
        "timestep": TIFA_TIMESTEP,
        "hop_size": TIFA_HOP,
        "fft_size": 2048,
        "win_size": 2048,
        "num_mels": TIFA_MELS,
        "vocab_size": TIFA_VOCAB,
    }
    # The ids zero, one and two stay out of the table: they are reserved, and a symbol that carried
    # one of them would be a symbol a host could not tell from padding or from the separator.
    vocabulary = {"symbols": dict(TIFA_SYMBOLS)}
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "config.json").write_text(json.dumps(config, indent=4) + "\n", encoding="utf-8")
    (directory / "vocabulary.json").write_text(
        json.dumps(vocabulary, indent=4) + "\n", encoding="utf-8"
    )
    for language, name in files.items():
        if language in absent:
            continue
        dictionary = directory / name
        dictionary.parent.mkdir(parents=True, exist_ok=True)
        dictionary.write_text(
            "".join(f"{entry}\n" for entry in TIFA_DICTIONARY_ENTRIES[language]), encoding="utf-8"
        )
    configuration = {
        "spectrogram": "../../spectrogram.onnx",
        "model": "../../model.onnx",
        "prepare": "../../prepare.onnx",
        "score": "../../score.onnx",
        "select": "../../select.onnx",
        "config": "../../config.json",
        "vocabulary": "../../vocabulary.json",
        "languages": {language: code for language, (_, code) in TIFA_LANGUAGES.items()},
    }
    for language, name in files.items():
        configuration[f"dictionary{language.capitalize()}"] = f"../../{name}"
    return configuration


# The numpy reading of what the five graphs above compute, written out separately from the graphs so
# that the two can be run against each other. A graph that agrees with a signature but not with its
# own description would let a test pass against a model nobody has.
def tifa_reference_spectrogram(waveform: np.ndarray, duration: np.ndarray) -> dict:
    """The grid the spectrogram graph writes: one hop mean per frame, and the frames still wanted."""
    frames = waveform.shape[1] // TIFA_HOP
    hops = waveform[:, : frames * TIFA_HOP].reshape(waveform.shape[0], frames, TIFA_HOP)
    means = hops.mean(axis=2, dtype=np.float32)
    spectrogram = np.repeat(means[:, :, None], TIFA_MELS, axis=2)
    # Round is ties to even, the convention torch.round uses too.
    wanted = np.round(duration / np.float32(TIFA_TIMESTEP)).astype(np.int64)
    index = np.arange(frames, dtype=np.int64)
    return {"spectrogram": spectrogram, "maskT": index[None, :] < wanted[:, None]}


def tifa_reference_model(
    spectrogram: np.ndarray, tokens: np.ndarray, maskT: np.ndarray, maskN: np.ndarray
) -> dict:
    """The ramp and the one-hot the model graph writes. The masks reach neither of its answers."""
    batch, frames, _ = spectrogram.shape
    count = tokens.shape[1]
    frame_share = np.arange(frames, dtype=np.float32) / np.float32(frames)
    token_share = np.arange(count, dtype=np.float32) / np.float32(count)
    distance = np.abs(frame_share[:, None] - token_share[None, :]).astype(np.float32)
    ramp = (np.float32(1.0) - distance).astype(np.float32)
    similarities = np.broadcast_to(ramp, (batch, frames, count)).copy()
    logits = np.zeros((batch, count, TIFA_VOCAB), dtype=np.float32)
    logits[np.arange(batch)[:, None], np.arange(count)[None, :], tokens] = TIFA_LOGIT_SCALE
    return {"similarities": similarities, "logits": logits}


def tifa_reference_prepare(
    paths: np.ndarray, words: np.ndarray, candidates: np.ndarray, grouped: np.ndarray
) -> dict:
    """The template the prepare graph writes: which rows differ, and how they group into segments."""
    batch = paths.shape[0]
    filled = paths != 0
    # A row differs when the slots it fills do not all hold the same token, and a row that fills none
    # agrees with itself.
    biggest = np.where(filled, paths, 0).max(axis=2)
    smallest = np.where(filled, paths, np.iinfo(np.int64).max).min(axis=2)
    differs = (filled.sum(axis=2) > 0) & (smallest != biggest)
    mapping = np.where(differs, np.cumsum(differs, axis=1), 0)
    earlier_differs = np.concatenate([np.zeros((batch, 1), dtype=bool), differs[:, :-1]], axis=1)
    earlier_words = np.concatenate([np.zeros((batch, 1), dtype=np.int64), words[:, :-1]], axis=1)
    starts = differs & ~(earlier_differs & (earlier_words == words))
    segments = np.where(differs, np.cumsum(starts, axis=1), 0)
    return {"tokens": paths[:, :, 0], "segments": segments, "mapping": mapping}


def tifa_reference_score(
    logits: np.ndarray,
    paths: np.ndarray,
    words: np.ndarray,
    segments: np.ndarray,
    mapping: np.ndarray,
) -> dict:
    """The fragment table the score graph writes. The costs carry no opinion about a candidate."""
    batch, _, columns = paths.shape
    fragments = np.unique(mapping)
    fragment_count = fragments.shape[0]
    member = mapping[:, :, None] == fragments[None, None, :]
    descriptors = np.stack(
        [
            np.where(member, words[:, :, None], 0).max(axis=1),
            np.where(member, segments[:, :, None], 0).max(axis=1),
        ],
        axis=2,
    ).astype(np.int64)
    lengths = np.einsum("bpf,bpc->bfc", member.astype(np.int64), (paths != 0).astype(np.int64))
    segment_ids = np.arange(1, fragment_count + 1, dtype=np.int64)
    capacity = (segments[:, :, None] == segment_ids[None, None, :]).sum(axis=1)
    return {
        "descriptors": descriptors,
        "lengths": lengths.astype(np.int64),
        "costs": np.zeros((batch, fragment_count, columns, fragment_count), dtype=np.float32),
        "tails": np.zeros((batch, fragment_count, fragment_count), dtype=np.float32),
        "capacity": capacity.astype(np.int64),
    }


def tifa_reference_select(
    paths: np.ndarray, words: np.ndarray, groups: np.ndarray, choices: np.ndarray
) -> dict:
    """The chosen token, word and group per row, and the mask that says which rows have a token.

    A row reads its word's choice, which numbers the candidate columns from one, and takes that
    column's token; zero means the word was not read, and a row whose column holds no token is a row
    the decode has nothing to place. Either way the row reports no word, which is what keeps a zero
    out of the phoneme sequence the decode aggregates onto the words the caller asked for.
    """
    batch, rows, columns = paths.shape
    word_count = choices.shape[1]
    best_tokens = np.zeros((batch, rows), dtype=np.int64)
    best_words = np.zeros((batch, rows), dtype=np.int64)
    best_groups = np.zeros((batch, rows), dtype=np.int64)
    for row in range(batch):
        for slot in range(rows):
            word = int(words[row, slot])
            if not 1 <= word <= word_count:
                continue
            choice = int(choices[row, word - 1])
            if choice == 0:
                continue
            column = min(max(choice - 1, 0), columns - 1)
            token = int(paths[row, slot, column])
            if token == 0:
                continue
            best_tokens[row, slot] = token
            best_words[row, slot] = word
            best_groups[row, slot] = int(groups[row, slot, column])
    return {
        "best_tokens": best_tokens,
        "best_words": best_words,
        "best_groups": best_groups,
        "maskN": best_tokens != 0,
    }


# What the real export declares, tensor by tensor: the names, element types and axis symbols a host
# keys its inputs and its results by. The fixture's graphs carry these verbatim, because a fixture
# that renamed or reshaped anything would be testing a contract the model does not have.
TIFA_SIGNATURES = {
    "spectrogram.onnx": {
        "inputs": [
            ("waveform", TensorProto.FLOAT, ["B", "L"]),
            ("duration", TensorProto.FLOAT, ["B"]),
        ],
        "outputs": [
            ("spectrogram", TensorProto.FLOAT, ["B", "T", TIFA_MELS]),
            ("maskT", TensorProto.BOOL, ["B", "T"]),
        ],
    },
    "model.onnx": {
        "inputs": [
            ("spectrogram", TensorProto.FLOAT, ["B", "T", TIFA_MELS]),
            ("tokens", TensorProto.INT64, ["B", "N"]),
            ("maskT", TensorProto.BOOL, ["B", "T"]),
            ("maskN", TensorProto.BOOL, ["B", "N"]),
        ],
        "outputs": [
            ("similarities", TensorProto.FLOAT, ["B", "T", "N"]),
            ("logits", TensorProto.FLOAT, ["B", "N", TIFA_VOCAB]),
        ],
    },
    "prepare.onnx": {
        "inputs": [
            ("paths", TensorProto.INT64, ["B", "P", "C"]),
            ("words", TensorProto.INT64, ["B", "P"]),
            ("candidates", TensorProto.BOOL, ["B", "W", "C"]),
            # The grouped flag is a zero dimensional scalar, not a one element vector: a host that
            # hands over the wrong rank is refused by the runtime rather than by the graph.
            ("grouped", TensorProto.BOOL, []),
        ],
        "outputs": [
            ("tokens", TensorProto.INT64, ["B", "P"]),
            ("segments", TensorProto.INT64, ["B", "P"]),
            ("mapping", TensorProto.INT64, ["B", "P"]),
        ],
    },
    "score.onnx": {
        "inputs": [
            ("logits", TensorProto.FLOAT, ["B", "P", TIFA_VOCAB]),
            ("paths", TensorProto.INT64, ["B", "P", "C"]),
            ("words", TensorProto.INT64, ["B", "P"]),
            ("segments", TensorProto.INT64, ["B", "P"]),
            ("mapping", TensorProto.INT64, ["B", "P"]),
        ],
        # The export annotates descriptors with two axes while the graph produces three, because
        # its dynamic axes rewrote the annotation. The host has to read the tensor the graph writes,
        # so this fixture declares, and produces, the rank the export actually returns.
        "outputs": [
            ("descriptors", TensorProto.INT64, ["B", "P1", 2]),
            ("lengths", TensorProto.INT64, ["B", "P1", "C"]),
            ("costs", TensorProto.FLOAT, ["B", "P1", "C", "P1"]),
            ("tails", TensorProto.FLOAT, ["B", "P1", "P1"]),
            ("capacity", TensorProto.INT64, ["B", "P1"]),
        ],
    },
    "select.onnx": {
        "inputs": [
            ("paths", TensorProto.INT64, ["B", "P", "C"]),
            ("words", TensorProto.INT64, ["B", "P"]),
            ("groups", TensorProto.INT64, ["B", "P", "C"]),
            ("choices", TensorProto.INT64, ["B", "W"]),
        ],
        "outputs": [
            ("best_tokens", TensorProto.INT64, ["B", "P"]),
            ("best_words", TensorProto.INT64, ["B", "P"]),
            ("best_groups", TensorProto.INT64, ["B", "P"]),
            ("maskN", TensorProto.BOOL, ["B", "P"]),
        ],
    },
}


def _check_round_is_ties_to_even() -> None:
    """Asserts that the Round the frame mask is cut with sends a half to the even integer.

    torch.round, which the export was built on, does that; a hand written port reaching for
    std::round rounds a half away from zero instead. The two agree everywhere else, so the rule is
    pinned down here, where it is cheap, rather than left to appear as one frame of disagreement in
    a test that is nowhere near this convention.
    """
    graph = helper.make_graph(
        [helper.make_node("Round", ["x"], ["y"])],
        "round_convention",
        [helper.make_tensor_value_info("x", TensorProto.FLOAT, [5])],
        [helper.make_tensor_value_info("y", TensorProto.FLOAT, [5])],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", TIFA_OPSET)])
    model.ir_version = 10
    halves = np.array([0.5, 1.5, 2.5, -0.5, -1.5], dtype=np.float32)
    even = np.array([0.0, 2.0, 2.0, 0.0, -2.0], dtype=np.float32)
    rounded = ReferenceEvaluator(model).run(None, {"x": halves})[0]
    if not np.array_equal(rounded, even):
        raise AssertionError(f"Round({halves.tolist()}) is {rounded.tolist()}, not {even.tolist()}")


def _declared_shape(value) -> tuple:
    """The name, element type and axes a declaration carries, with axis symbols kept as names."""
    dims = [
        dim.dim_param if dim.HasField("dim_param") else dim.dim_value
        for dim in value.type.tensor_type.shape.dim
    ]
    return value.name, value.type.tensor_type.elem_type, dims


def _check_signature(where: str, model: onnx.ModelProto, signature: dict) -> None:
    """Asserts that a fixture graph declares the tensors, and the operator set, the export declares."""
    for side, field in (("inputs", "input"), ("outputs", "output")):
        declared = [_declared_shape(value) for value in getattr(model.graph, field)]
        expected = [(name, kind, list(shape)) for name, kind, shape in signature[side]]
        if declared != expected:
            raise AssertionError(f"{where}: {side} are {declared}, the export declares {expected}")
    opset = model.opset_import[0].version
    if opset != TIFA_OPSET:
        raise AssertionError(f"{where}: operator set {opset}, the export declares {TIFA_OPSET}")


def _check_no_dead_tensors(where: str, model: onnx.ModelProto) -> None:
    """Asserts that nothing in a graph is computed and then never read.

    A leftover constant does not change an answer, but it is not invisible to a runtime either:
    onnxruntime prunes it and says so, which turns the fixture into noise in the log of every host
    that loads it. Better that the script that wrote it says it first.
    """
    read = {name for node in model.graph.node for name in node.input if name}
    declared = {value.name for value in model.graph.output}
    written = [name for node in model.graph.node for name in node.output if name]
    dead = sorted(name for name in written if name not in read and name not in declared)
    if dead:
        raise AssertionError(f"{where}: {dead} are written and never read")


def _assert_tensor(where: str, name: str, got: np.ndarray, want: np.ndarray) -> None:
    """Asserts one output of a fixture graph against the numpy reading of the same arithmetic."""
    if got.shape != want.shape:
        raise AssertionError(f"{where}: {name} is {got.shape}, the reference says {want.shape}")
    if got.dtype != want.dtype:
        raise AssertionError(f"{where}: {name} is {got.dtype}, the reference says {want.dtype}")
    if np.issubdtype(want.dtype, np.floating):
        # A cost a graph writes where nothing is reachable may be an infinity, and equal infinities
        # compare equal. The tolerance is for the finite values, which are sums in single precision.
        agrees = np.allclose(got, want, rtol=1e-6, atol=1e-6, equal_nan=True)
    else:
        agrees = np.array_equal(got, want)
    if not agrees:
        raise AssertionError(f"{where}: {name} differs from the reference:\n{got}\n{want}")


def verify_tifa(directory: Path) -> None:
    """Runs the five graphs the way a host will and asserts they mean what this script says they do.

    Nothing else can tell a fixture's arithmetic from its signature, so this is where the two are
    tied together: every graph is loaded, its declared tensors are compared with the export's, and it
    is then run by onnx's reference evaluator on a small grid whose answer the numpy implementation
    above writes down independently -- one batch, four template rows, two candidates, two words, two
    candidates per word, sixteen frames and four tokens. The graph that reads a word's chosen column
    is run twice, with the second word read and then not read, so that the choice of zero is answered
    as well. A drift between the graph and its meaning fails the build here instead of passing a test
    that only compares shapes.
    """
    _check_round_is_ties_to_even()
    # One batch, P = 4 rows, C = 2 candidate columns, W = 2 words, T = 16 frames, N = 4 tokens.
    waveform = np.linspace(-1.0, 1.0, 16 * TIFA_HOP, dtype=np.float32).reshape(1, -1)
    duration = np.array([0.123], dtype=np.float32)
    paths = np.array([[[5, 5], [7, 9], [3, 4], [0, 4]]], dtype=np.int64)
    words = np.array([[1, 1, 2, 2]], dtype=np.int64)
    groups = np.array([[[1, 1], [1, 2], [1, 1], [1, 1]]], dtype=np.int64)
    candidates = np.array([[[True, False], [True, True]]], dtype=bool)
    grouped = np.array(False, dtype=bool)
    # The first candidate column of each word, then the same grid with the first word left unread.
    choices = [np.array([[1, 1]], dtype=np.int64), np.array([[0, 1]], dtype=np.int64)]

    prepared = tifa_reference_prepare(paths, words, candidates, grouped)
    front = tifa_reference_spectrogram(waveform, duration)
    modelled = tifa_reference_model(
        front["spectrogram"], prepared["tokens"], front["maskT"], prepared["tokens"] != 0
    )
    scored = tifa_reference_score(
        modelled["logits"], paths, words, prepared["segments"], prepared["mapping"]
    )

    cases = {
        "spectrogram.onnx": (
            {"waveform": waveform, "duration": duration},
            front,
        ),
        "model.onnx": (
            {
                "spectrogram": front["spectrogram"],
                "tokens": prepared["tokens"],
                "maskT": front["maskT"],
                "maskN": prepared["tokens"] != 0,
            },
            modelled,
        ),
        "prepare.onnx": (
            {"paths": paths, "words": words, "candidates": candidates, "grouped": grouped},
            prepared,
        ),
        "score.onnx": (
            {
                "logits": modelled["logits"],
                "paths": paths,
                "words": words,
                "segments": prepared["segments"],
                "mapping": prepared["mapping"],
            },
            scored,
        ),
        "select.onnx": (
            {"paths": paths, "words": words, "groups": groups, "choices": choices[0]},
            tifa_reference_select(paths, words, groups, choices[0]),
        ),
        "select-none.onnx": (
            {"paths": paths, "words": words, "groups": groups, "choices": choices[1]},
            tifa_reference_select(paths, words, groups, choices[1]),
        ),
    }

    checked = 0
    for name, signature in TIFA_SIGNATURES.items():
        where = f"{directory.name}/{name}"
        model = onnx.load(str(directory / name))
        _check_signature(where, model, signature)
        _check_no_dead_tensors(where, model)
        evaluator = ReferenceEvaluator(model)
        # The graph that reads a word's chosen column runs a second time, on the same grid with the
        # first word left unread: a grid whose every word was read never reaches that branch.
        runs = [(cases[name], "")]
        if name == "select.onnx":
            runs.append((cases["select-none.onnx"], ", first word unread"))
        for (feed, expected), suffix in runs:
            produced = evaluator.run(None, feed)
            for (output, _, _), value in zip(signature["outputs"], produced):
                _assert_tensor(f"{where}{suffix}", output, value, expected[output])
                checked += 1
    print(f"checked {checked} tensors of {len(TIFA_SIGNATURES)} graphs in {directory.name}")


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
    parser.add_argument("--output", default="build/cmake/fixtures", type=Path,
                        help="output directory, replaced if it exists "
                             "(default: build/cmake/fixtures, the directory the tests read when the "
                             "build directory is the build/cmake of the README)")
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

    def align_declaration(exports: dict, configuration: dict, variant: str = "hfa",
                          name: str = "Fixture Align", defaults: dict = None) -> dict:
        """Returns one align fixture's declaration, with \a exports merged into its exports.

        Every hfa fixture declares the same format, languages and knobs, so those are written here.
        A variant that declares others states its \a variant, its \a name and what it declares
        instead, which keeps the merge and the removal of a key that is set to None in one place for
        both: the tifa graphs run at another rate and in another set of languages.
        """
        if defaults is None:
            defaults = {
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
            }
        declaration = {
            "interface": "org.openvpi.otter.inference.Align",
            "level": 1,
            "variant": variant,
            "name": name,
            "exports": dict(defaults),
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
    # The reference implementation assumes that it is. A provider with the same assumption would
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
    # checks the declaration against them, so each package carries one value that contradicts one
    # of two things: a file of the model, or, where no file states the value, the variant that
    # reads the declaration. If the provider omits the corresponding check, the package loads and
    # the test fails.
    align_rejects = []
    for name, exports, files in (
        # One channel is what this variant supports, and no file of the model's own states it, so
        # the count is the one value of the declaration that contradicts the variant rather than
        # the model the declaration is paired with.
        ("two-channels", {"channelCount": 2}, {}),
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

    def tifa_language(language: str, phonemes: list = None) -> dict:
        """A languages entry of the tifa declaration, with \a phonemes in place of the model's own.

        The declaration and the export's vocabulary have to agree symbol for symbol, which is what
        makes a list that has drifted by one entry worth a package of its own.
        """
        scheme, code = TIFA_LANGUAGES[language]
        if phonemes is None:
            phonemes = [
                label.split("/", 1)[1] for label in TIFA_SYMBOLS if label.startswith(code + "/")
            ]
        return {"language": language, "scheme": scheme, "lyrics": "scheme", "phonemes": phonemes}

    def tifa_exports() -> dict:
        """Returns the align exports the tifa fixtures share.

        The model detects no silence and no breath: it carries AP, EP and GS without a language
        prefix, and the label the host fills a gap with is this variant's own spelling. That is
        exactly why it must not be one of the phonemes a declared language lists.
        """
        return {
            "sampleRate": TIFA_RATE,
            "channelCount": 1,
            "maxSegmentDuration": 60.0,
            "languages": [tifa_language(language) for language in TIFA_LANGUAGES],
            "defaultLanguage": "cmn",
            "silenceLabel": "SP",
        }

    def tifa_graphs(directory: Path, **files) -> dict:
        """Returns the five tifa graphs and the configuration block that names them.

        \a files are the model files that must contradict the declaration they are paired with.
        """
        build_tifa_spectrogram(directory / "spectrogram.onnx")
        build_tifa_model(directory / "model.onnx")
        build_tifa_prepare(directory / "prepare.onnx")
        build_tifa_score(directory / "score.onnx")
        build_tifa_select(directory / "select.onnx")
        return write_tifa_model_files(directory, **files)

    tifa = output / "fixture-align-tifa"
    write_package(
        tifa,
        "otter/fixture-align-tifa",
        {"align": align_declaration({}, tifa_graphs(tifa), variant="tifa",
                                    name="Fixture TIFA Align", defaults=tifa_exports())},
    )
    # The graphs are arithmetic this script wrote down twice, so it runs the two against each other
    # as it writes them: a graph that stopped meaning what the numpy reading says fails the build.
    verify_tifa(tifa)

    tifa_rejects = []
    # The names carry the variant before the defect, which is how the test that loads them looks for
    # them, and every defect here is one the provider refuses at load rather than at the end of a run.
    for name, exports, files in (
        # The rate the graphs name and the rate the model's own config.json states, told apart: a
        # host preparing audio at the declared rate aligns at the wrong speed, and never fails.
        ("wrong-rate", {}, {"rate": 16000}),
        # A language the declaration lists and the configuration gives no dictionary: nothing can be
        # expanded into phonemes, however sound the rest of the declaration looks.
        ("no-dictionary", {}, {"dictionaries": {"eng": "dictionaries/fixture-eng.txt"}}),
        # The same, told the other way: the key is there and the file it names is not.
        ("missing-dictionary", {}, {"absent": ("cmn",)}),
        # A list that differs in both directions at once, because either one alone is a different
        # mistake: a phoneme the model can emit and the caller would never see, and one it cannot.
        ("wrong-phonemes",
         {"languages": [tifa_language("cmn", ["a", "c", "d", "e", "f", "g", "x"]),
                        tifa_language("eng")]},
         {}),
        # A label a host filters the gaps out by, set to a phoneme of a listed language, which would
        # filter that phoneme out of the result as well.
        ("unknown-silence", {"silenceLabel": "a"}, {}),
        # A dictionary for a language the declaration does not list: nothing reads it, so the load
        # is allowed and the lint is what says so.
        ("extra-dictionary", {"languages": [tifa_language("eng")], "defaultLanguage": "eng"}, {}),
        # What the three below promise is a capability of the variant rather than a value the
        # model's own files carry, so no argument of write_tifa_model_files() can contradict them
        # and the declaration is refused where it is read.
        #
        # The five graphs take one channel each, so a declaration of two asks the host to prepare
        # audio that the variant would have to mix down before its model saw it.
        ("two-channels", {"channelCount": 2}, {}),
        # Another notation is refused rather than read as the scheme it is called by: the same
        # language written in another notation carries other phonemes.
        ("unknown-scheme",
         {"languages": [dict(tifa_language("cmn"), scheme="x-sampa"), tifa_language("eng")]},
         {}),
        # The model has no non-speech head, so it can report neither a breath nor a cough:
        # promising one would make "none found" and "nothing was looked for" the same answer.
        ("promised-non-speech", {"nonSpeechPhonemes": ["AP"]}, {}),
    ):
        root = output / f"fixture-align-tifa-{name}"
        tifa_rejects.append(root)
        write_package(
            root,
            f"otter/fixture-align-tifa-{name}",
            {"align": align_declaration(exports, tifa_graphs(root, **files), variant="tifa",
                                        name="Fixture TIFA Align", defaults=tifa_exports())},
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

    # The graph takes one channel, so a declaration of two describes audio that the provider would
    # have to mix down before the model saw it. The graph is the one the packages above carry, so
    # the declared count is the only defect.
    rmvpe_two_channels = output / "fixture-rmvpe-two-channels"
    build_rmvpe(rmvpe_two_channels / "inferences" / "f0" / "rmvpe.onnx")
    write_package(
        rmvpe_two_channels,
        "otter/fixture-rmvpe-two-channels",
        {
            "f0": {
                "interface": "org.openvpi.otter.inference.F0",
                "level": 1,
                "variant": "rmvpe",
                "name": "Fixture RMVPE declaring two channels",
                "exports": {"sampleRate": RMVPE_RATE, "interval": 0.01, "channelCount": 2},
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

    # The models take one channel each, so a declaration of two describes audio that the provider
    # would have to mix down before the encoder saw it. The four models are written as for the
    # packages above, so the declared count is the only defect.
    note_two_channels = output / "fixture-note-two-channels"
    write_package(
        note_two_channels,
        "otter/fixture-note-two-channels",
        {
            "note": {
                "interface": "org.openvpi.otter.inference.Note",
                "level": 1,
                "variant": "game",
                "name": "Fixture Note declaring two channels",
                "exports": {"sampleRate": NOTE_RATE, "channelCount": 2, "languages": ["zxx"],
                            "defaultLanguage": "zxx"},
                "configuration": note_models(note_two_channels / "inferences" / "note"),
            }
        },
    )

    # This package loads: the exports promise no alignment, which the variant accepts, since only a
    # declaration that promises alignment while the model is missing is rejected at load. The model
    # is present all the same, so an execution that receives known notes is refused by that
    # declaration alone, which is the case this package exists for.
    no_known_notes = output / "fixture-note-no-known-notes"
    no_known_notes_models = note_models(no_known_notes / "inferences" / "note")
    build_note_dur2bd(no_known_notes / "inferences" / "note" / "dur2bd.onnx")
    # note_models() numbers boundaryToDuration, the other direction of the same pair. The alignment
    # path is the optional durationToBoundary, and the graph written above is its file here.
    no_known_notes_models["durationToBoundary"] = "./dur2bd.onnx"
    write_package(
        no_known_notes,
        "otter/fixture-note-no-known-notes",
        {
            "note": {
                "interface": "org.openvpi.otter.inference.Note",
                "level": 1,
                "variant": "game",
                "name": "Fixture Note that promises no alignment on known notes",
                "exports": {"sampleRate": NOTE_RATE, "languages": ["zxx"],
                            "defaultLanguage": "zxx", "supportsKnownNotes": False},
                "configuration": no_known_notes_models,
            }
        },
    )

    print(f"wrote {rmvpe}")
    print(f"wrote {note} and {no_known_notes}")
    print(f"wrote {align}, {reordered} and {tifa}")
    print(f"wrote {wrong_rate}, {unnumbered}, {no_alignment}, {no_default}, "
          f"{rmvpe_two_channels} and {note_two_channels}, which must not load")
    print(f"wrote {', '.join(str(root) for root in align_rejects + tifa_rejects)}, "
          "which must not load")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
