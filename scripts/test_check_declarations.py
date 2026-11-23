"""Tests for check-declarations.py, the package lint.

The lint is the last check before a declaration is released, so these tests fix its verdicts: a
valid package of each contract must pass without findings, and every kind of inconsistency between
the two blocks of a declaration must be reported. Run with python -m unittest discover -s scripts.
"""

import contextlib
import copy
import importlib.util
import io
import json
import re
import shutil
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load_lint():
    spec = importlib.util.spec_from_file_location("check_declarations",
                                                  HERE / "check-declarations.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


lint = load_lint()

F0_EXPORTS = {
    "sampleRate": 16000,
    "channelCount": 1,
    "interval": 0.01,
    "knobs": {
        "voicingThreshold": {"minimum": 0.0, "maximum": 1.0, "default": 0.03},
        "interpolateUnvoiced": {"default": True},
    },
}

NOTE_EXPORTS = {
    "sampleRate": 44100,
    "languages": ["zxx"],
    "defaultLanguage": "zxx",
    "supportsKnownNotes": True,
    "knobs": {"steps": {"minimum": 1, "maximum": 1000, "default": 8}},
}

NOTE_CONFIGURATION = {
    "encoder": "./encoder.onnx",
    "segmenter": "./segmenter.onnx",
    "estimator": "./estimator.onnx",
    "boundaryToDuration": "./bd2dur.onnx",
    "durationToBoundary": "./dur2bd.onnx",
    "timestep": 0.01,
    "languages": {"zxx": 0},
}

ALIGN_EXPORTS = {
    "sampleRate": 44100,
    "channelCount": 1,
    "maxSegmentDuration": 60,
    "languages": [{"language": "zxx", "scheme": "test", "lyrics": "scheme", "phonemes": ["a"]}],
    "defaultLanguage": "zxx",
    "nonSpeechPhonemes": ["AP", "EP"],
    "defaultNonSpeechPhonemes": ["AP"],
    "silenceLabel": "SP",
    "knobs": {
        "nonSpeechThreshold": {"minimum": 0.0, "maximum": 1.0, "default": 0.5},
        "nonSpeechMinDuration": {"minimum": 0.0, "maximum": 2.0, "default": 0.1},
        "gapFill": {"minimum": 0.0, "maximum": 1.0, "default": 0.1},
    },
}

ALIGN_CONFIGURATION = {
    "model": "../../model.onnx",
    "config": "../../config.json",
    "vocab": "../../vocab.json",
    "languages": {"zxx": "zx"},
}

ALIGN_CONFIG_JSON = {"mel_spec_config": {"sample_rate": 44100, "hop_size": 441}}

ALIGN_VOCAB_JSON = {
    "vocab": {"zx/a": 1, "SP": 0, "AP": 0, "EP": 0},
    "silent_phonemes": ["SP", "AP", "EP"],
    "non_lexical_phonemes": ["AP", "EP"],
    "dictionaries": {"zx": "zx_dict.txt"},
}

TIFA_EXPORTS = {
    "sampleRate": 48000,
    "channelCount": 1,
    "maxSegmentDuration": 60,
    "languages": [
        {"language": "cmn", "scheme": "pinyin", "lyrics": "scheme", "phonemes": ["AP", "a", "b"]}
    ],
    "defaultLanguage": "cmn",
    "silenceLabel": "SP",
}

TIFA_CONFIGURATION = {
    "spectrogram": "../../spectrogram.onnx",
    "model": "../../model.onnx",
    "prepare": "../../prepare.onnx",
    "score": "../../score.onnx",
    "select": "../../select.onnx",
    "config": "../../config.json",
    "vocabulary": "../../vocabulary.json",
    "dictionaryCmn": "../../zh_dict.txt",
    "languages": {"cmn": "zh"},
}

TIFA_CONFIG_JSON = {
    "samplerate": 48000,
    "timestep": 0.01,
    "hop_size": 480,
    "fft_size": 2048,
    "win_size": 2048,
    "num_mels": 80,
    "vocab_size": 256,
}

TIFA_VOCABULARY_JSON = {"symbols": {"AP": 3, "zh/a": 4, "zh/b": 5}}


class Packages(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="otter-lint-"))

    def tearDown(self):
        shutil.rmtree(self.root)

    def write(self, name, interface, variant, exports, configuration, contribution="f0",
              desc=None):
        package = self.root / name
        directory = package / "inferences" / contribution
        directory.mkdir(parents=True)
        manifest = {
            "$version": "1.0",
            "id": f"test/{name}",
            "version": "1.0.0.0",
            "compatVersion": "1.0.0.0",
            "runtimeLevel": 1,
            "contributions": {
                "inference": [{"id": contribution, "path": f"./inferences/{contribution}/inference.json"}]
            },
        }
        manifest.update(desc or {})
        # The declarations are written with LF line endings, which the lint requires: write_bytes
        # writes the encoded bytes without the line-ending translation that text mode applies, so
        # a fixture with CRLF line endings would be rejected for a reason the case does not test.
        (package / "desc.json").write_bytes(json.dumps(manifest).encode("utf-8"))
        (directory / "inference.json").write_bytes(json.dumps({
            "interface": interface,
            "level": 1,
            "variant": variant,
            "name": name,
            "exports": exports,
            "configuration": configuration,
        }).encode("utf-8"))
        # The lint requires every path named by a configuration to exist and be non-empty.
        for key, value in configuration.items():
            if isinstance(value, str) and value.endswith(".onnx"):
                (directory / value).write_bytes(b"\0")
        return package

    def check(self, package, require_project=False):
        # The synthetic packages of these tests are built in a temporary directory with no
        # CMakeLists.txt above them, so a version comparison against a project is not what they
        # exercise. The tests that do exercise it build a project of their own.
        report = lint.Report()
        lint.check_package(package, report, require_project=require_project)
        return report

    def write_align(self, name, exports=None, configuration=None, config_json=None, vocab=None,
                    desc=None):
        """Writes a package with the aligner's layout, with the model's own files at the root.

        The aligner reads its vocabulary and its mel spectrogram configuration from the model's
        export instead of from the declaration, and locates one dictionary per language through
        the vocabulary. A package of this contract can therefore contain defects that the
        declaration alone does not reveal, and these cases cover such defects.
        """
        package = self.write(name, lint.ALIGN, "hfa", exports or ALIGN_EXPORTS,
                             configuration or ALIGN_CONFIGURATION, "align", desc)
        (package / "model.onnx").write_bytes(b"\0")
        (package / "config.json").write_text(json.dumps(config_json or ALIGN_CONFIG_JSON),
                                             encoding="utf-8")
        (package / "vocab.json").write_text(json.dumps(vocab or ALIGN_VOCAB_JSON),
                                            encoding="utf-8")
        (package / "zx_dict.txt").write_text("a\tzx/a\n", encoding="utf-8")
        return package

    def write_tifa(self, name, exports=None, configuration=None, config_json=None,
                   vocabulary=None):
        """Writes a package shaped like tifa's: five graphs, an exported config and a symbol table.

        tifa's export carries the audio timing and the symbols; the dictionaries are the
        declaration's own, one per declared language, because the export does not name them.
        """
        configuration = configuration or TIFA_CONFIGURATION
        package = self.write(name, lint.ALIGN, "tifa", exports or TIFA_EXPORTS, configuration,
                             "align")
        for value in configuration.values():
            if isinstance(value, str) and value.endswith(".txt"):
                (package / "inferences" / "align" / value).write_text("a\ta\n", encoding="utf-8")
        (package / "config.json").write_text(json.dumps(config_json or TIFA_CONFIG_JSON),
                                             encoding="utf-8")
        (package / "vocabulary.json").write_text(json.dumps(vocabulary or TIFA_VOCABULARY_JSON),
                                                 encoding="utf-8")
        return package

    def test_align_exports_must_match_the_model(self):
        exports = dict(ALIGN_EXPORTS)
        exports["sampleRate"] = 16000
        report = self.check(self.write_align("wrongrate", exports=exports))
        self.assertGreater(report.errors, 0)
    def test_align_language_needs_a_dictionary(self):
        configuration = dict(ALIGN_CONFIGURATION)
        configuration["languages"] = {"zxx": "qq"}
        report = self.check(self.write_align("nodict", configuration=configuration))
        self.assertGreater(report.errors, 0)

        vocab = json.loads(json.dumps(ALIGN_VOCAB_JSON))
        del vocab["dictionaries"]["zx"]
        report = self.check(self.write_align("nocode", vocab=vocab))
        self.assertGreater(report.errors, 0)

    def test_align_promises_must_be_in_the_vocabulary(self):
        exports = dict(ALIGN_EXPORTS)
        exports["nonSpeechPhonemes"] = ["AP", "BR"]
        report = self.check(self.write_align("nophoneme", exports=exports))
        self.assertGreater(report.errors, 0)

        exports = dict(ALIGN_EXPORTS)
        exports["silenceLabel"] = "SIL"
        report = self.check(self.write_align("nolabel", exports=exports))
        self.assertGreater(report.errors, 0)

    def test_align_language_needs_a_code(self):
        exports = dict(ALIGN_EXPORTS)
        exports["languages"] = ALIGN_EXPORTS["languages"] + [
            {"language": "eng", "scheme": "arpabet", "lyrics": "text", "phonemes": ["aa"]}]
        report = self.check(self.write_align("unmapped", exports=exports))
        self.assertGreater(report.errors, 0)

    def test_align_default_non_speech_must_be_promised(self):
        exports = dict(ALIGN_EXPORTS)
        exports["nonSpeechPhonemes"] = ["AP"]
        exports["defaultNonSpeechPhonemes"] = ["EP"]
        report = self.check(self.write_align("undefault", exports=exports))
        self.assertGreater(report.errors, 0)

    def test_align_phonemes_must_be_the_vocabulary(self):
        # The hfa interpreter compares the declared phonemes with the vocabulary in both directions.
        for phonemes in (["a", "b"], ["b"]):
            exports = json.loads(json.dumps(ALIGN_EXPORTS))
            exports["languages"][0]["phonemes"] = phonemes
            report = self.check(self.write_align("phonemes" + "".join(phonemes), exports=exports))
            self.assertGreater(report.errors, 0, phonemes)

    def test_align_language_entries_have_a_grammar(self):
        for field, value in (("language", "zh"), ("scheme", "Pin Yin"), ("lyrics", "romanized")):
            exports = json.loads(json.dumps(ALIGN_EXPORTS))
            exports["languages"][0][field] = value
            report = self.check(self.write_align("grammar" + field, exports=exports))
            self.assertGreater(report.errors, 0, field)

        exports = json.loads(json.dumps(ALIGN_EXPORTS))
        exports["languages"].append(dict(exports["languages"][0]))
        report = self.check(self.write_align("twice", exports=exports))
        self.assertGreater(report.errors, 0)

    def test_good_packages_pass_clean(self):
        rmvpe = self.write("rmvpe", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./rmvpe.onnx"})
        game = self.write("game", lint.NOTE, "game", NOTE_EXPORTS, NOTE_CONFIGURATION, "note")
        hfa = self.write_align("hfa")
        tifa = self.write_tifa("tifa")
        for package in (rmvpe, game, hfa, tifa):
            report = self.check(package)
            self.assertEqual((report.errors, report.warnings), (0, 0), package.name)

    def test_tifa_exports_must_match_the_export(self):
        config = dict(TIFA_CONFIG_JSON)
        config["samplerate"] = 44100
        report = self.check(self.write_tifa("wrongrate", config_json=config))
        self.assertGreater(report.errors, 0)

    def test_tifa_language_needs_its_dictionary(self):
        configuration = dict(TIFA_CONFIGURATION)
        del configuration["dictionaryCmn"]
        report = self.check(self.write_tifa("nodict", configuration=configuration))
        self.assertGreater(report.errors, 0)

        # A dictionary nothing declares is a warning: the package carries a file no execution
        # reaches, which usually means the exports lost a language.
        configuration = dict(TIFA_CONFIGURATION)
        configuration["dictionaryEng"] = "../../en_dict.txt"
        report = self.check(self.write_tifa("extradict", configuration=configuration))
        self.assertGreater(report.warnings, 0)

    def test_tifa_language_the_variant_does_not_cover(self):
        # The aligner resolves the dictionary of every language its exports declare through its own
        # table of the four languages it covers and rejects a declaration that lists another one
        # (tifa/main.cpp:77-82, :1610-1619). The lint skipped such a language, because its own
        # table has no entry either, and so passed a package that cannot load.
        exports = json.loads(json.dumps(TIFA_EXPORTS))
        exports["languages"][0]["language"] = "deu"
        exports["defaultLanguage"] = "deu"
        configuration = dict(TIFA_CONFIGURATION)
        configuration["languages"] = {"deu": "zh"}
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            report = self.check(self.write_tifa("uncovered", exports=exports,
                                                configuration=configuration))
        self.assertGreater(report.errors, 0)
        self.assertIn("which the tifa variant does not cover", output.getvalue())

    def test_tifa_phonemes_must_be_the_vocabulary(self):
        # The shared symbols carry no language prefix, so they may be promised by any language.
        # a prefixed symbol that is missing or unknown is an error either way.
        for phonemes in (["AP", "a"], ["AP", "a", "b", "c"]):
            exports = json.loads(json.dumps(TIFA_EXPORTS))
            exports["languages"][0]["phonemes"] = phonemes
            report = self.check(self.write_tifa("phonemes" + "".join(phonemes), exports=exports))
            self.assertGreater(report.errors, 0, phonemes)

    def test_tifa_silence_label_is_not_a_phoneme(self):
        exports = dict(TIFA_EXPORTS)
        exports["silenceLabel"] = "a"
        report = self.check(self.write_tifa("labelsilence", exports=exports))
        self.assertGreater(report.errors, 0)

    def test_exports_need_the_audio_format(self):
        exports = dict(F0_EXPORTS)
        del exports["sampleRate"]
        report = self.check(self.write("norate", lint.F0, "rmvpe", exports, {"model": "./m.onnx"}))
        self.assertGreater(report.errors, 0)

    def test_contract_facts_do_not_belong_in_configuration(self):
        report = self.check(self.write("misplaced", lint.F0, "rmvpe", F0_EXPORTS,
                                       {"model": "./m.onnx", "sampleRate": 16000}))
        self.assertGreater(report.errors, 0)

    def test_a_knob_default_must_lie_in_its_range(self):
        exports = json.loads(json.dumps(F0_EXPORTS))
        exports["knobs"]["voicingThreshold"] = {"minimum": 0.5, "maximum": 1.0, "default": 0.1}
        report = self.check(self.write("knob", lint.F0, "rmvpe", exports, {"model": "./m.onnx"}))
        self.assertGreater(report.errors, 0)

    def test_the_two_blocks_must_agree(self):
        exports = dict(NOTE_EXPORTS)
        exports["languages"] = ["zxx", "eng"]
        report = self.check(self.write("unnumbered", lint.NOTE, "game", exports,
                                       NOTE_CONFIGURATION, "note"))
        self.assertGreater(report.errors, 0)

        configuration = dict(NOTE_CONFIGURATION)
        del configuration["durationToBoundary"]
        report = self.check(self.write("noalign", lint.NOTE, "game", NOTE_EXPORTS,
                                       configuration, "note"))
        self.assertGreater(report.errors, 0)

    def test_listed_languages_need_a_default(self):
        # The contract's reader rejects this case at load. A package without a default that
        # passed the lint would be published and then fail to load on every host.
        exports = dict(NOTE_EXPORTS)
        del exports["defaultLanguage"]
        report = self.check(self.write("nodefault", lint.NOTE, "game", exports,
                                       NOTE_CONFIGURATION, "note"))
        self.assertGreater(report.errors, 0)

        exports = dict(NOTE_EXPORTS)
        exports["defaultLanguage"] = "eng"
        report = self.check(self.write("otherdefault", lint.NOTE, "game", exports,
                                       NOTE_CONFIGURATION, "note"))
        self.assertGreater(report.errors, 0)

    def test_languages_are_iso_639_3(self):
        exports = dict(NOTE_EXPORTS)
        exports["languages"] = ["zh"]
        exports["defaultLanguage"] = "zh"
        configuration = dict(NOTE_CONFIGURATION)
        configuration["languages"] = {"zh": 0}
        report = self.check(self.write("twoletter", lint.NOTE, "game", exports, configuration,
                                       "note"))
        self.assertGreater(report.errors, 0)

    def test_a_default_language_belongs_in_the_exports(self):
        configuration = dict(NOTE_CONFIGURATION)
        configuration["defaultLanguage"] = "zxx"
        report = self.check(self.write("configured", lint.NOTE, "game", NOTE_EXPORTS,
                                       configuration, "note"))
        self.assertGreater(report.errors, 0)

    def test_a_missing_model_file_is_an_error(self):
        package = self.write("nofile", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"})
        (package / "inferences" / "f0" / "m.onnx").unlink()
        self.assertGreater(self.check(package).errors, 0)

    def test_declarations_only_needs_no_model_files(self):
        # The declarations are checked in this mode before their models are in place, so it must
        # still report what is wrong with a declaration.
        package = self.write("nofiles", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"})
        (package / "inferences" / "f0" / "m.onnx").unlink()
        report = lint.Report()
        lint.check_package(package, report, declarations_only=True, require_project=False)
        self.assertEqual((report.errors, report.warnings), (0, 0))

        hfa = self.write_align("hfanofiles")
        for name in ("model.onnx", "config.json", "vocab.json", "zx_dict.txt"):
            (hfa / name).unlink()
        report = lint.Report()
        lint.check_package(hfa, report, declarations_only=True, require_project=False)
        self.assertEqual((report.errors, report.warnings), (0, 0))

        exports = dict(F0_EXPORTS)
        del exports["sampleRate"]
        broken = self.write("brokennofiles", lint.F0, "rmvpe", exports, {"model": "./m.onnx"})
        report = lint.Report()
        lint.check_package(broken, report, declarations_only=True, require_project=False)
        self.assertGreater(report.errors, 0)

    def test_declarations_must_use_lf_line_endings(self):
        # The packager copies a declaration into the archive byte for byte and hashes those bytes
        # into the manifest of the release, so a file saved with CRLF line endings ships as it is
        # and no other machine reproduces that digest. Both files of a declaration are checked.
        def relined(path, ending):
            # A one-line fixture carries no line separator to convert, so every document is given a
            # trailing one first: the lint reads bytes, and a file without a separator cannot carry
            # the CRLF this case is about.
            text = path.read_bytes().replace(b"\r\n", b"\n").rstrip(b"\n") + b"\n"
            path.write_bytes(text.replace(b"\n", ending))

        package = self.write("endings", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"})
        declaration = package / "inferences" / "f0" / "inference.json"
        self.assertEqual(self.check(package).errors, 0, "LF line endings pass")

        for path in (package / "desc.json", declaration):
            relined(path, b"\r\n")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                report = self.check(package)
            self.assertEqual(report.errors, 1, path.name)
            self.assertIn("LF", output.getvalue(), path.name)
            relined(path, b"\n")
        self.assertEqual(self.check(package).errors, 0, "LF line endings pass again")

    def test_what_the_loader_refuses_the_lint_refuses(self):
        # Each case applies one change to a valid package, and each changed package is rejected
        # at load by a contract reader, by a variant's interpreter or by the synthrt loader, as
        # the comment of the case states. A package that passes the lint must load, so the lint
        # must report an error for every case. The verdicts were verified against the loader when
        # the cases were written.
        def exports(base, **changes):
            result = copy.deepcopy(base)
            for key, value in changes.items():
                if value is None:
                    result.pop(key, None)
                else:
                    result[key] = value
            return result

        def configuration(base, **changes):
            return exports(base, **changes)

        vocab = copy.deepcopy(ALIGN_VOCAB_JSON)
        cases = {
            # The rmvpe interpreter: the graph runs at 16 kHz, a frame every 10 ms, one channel.
            "rmvpe-channels": lambda n: self.write(
                n, lint.F0, "rmvpe", exports(F0_EXPORTS, channelCount=2), {"model": "./m.onnx"}),
            "rmvpe-rate": lambda n: self.write(
                n, lint.F0, "rmvpe", exports(F0_EXPORTS, sampleRate=44100), {"model": "./m.onnx"}),
            "rmvpe-interval": lambda n: self.write(
                n, lint.F0, "rmvpe", exports(F0_EXPORTS, interval=0.02), {"model": "./m.onnx"}),
            # readPositiveInt: an int, so nothing larger than 2^31 - 1.
            "rate-out-of-range": lambda n: self.write(
                n, lint.F0, "rmvpe", exports(F0_EXPORTS, sampleRate=2**31), {"model": "./m.onnx"}),
            # The game interpreter: 44.1 kHz and one channel.
            "game-rate": lambda n: self.write(
                n, lint.NOTE, "game", exports(NOTE_EXPORTS, sampleRate=16000),
                NOTE_CONFIGURATION, "note"),
            "game-channels": lambda n: self.write(
                n, lint.NOTE, "game", exports(NOTE_EXPORTS, channelCount=2),
                NOTE_CONFIGURATION, "note"),
            # readPositiveDouble and readUnitDouble on the game configuration.
            "game-timestep": lambda n: self.write(
                n, lint.NOTE, "game", NOTE_EXPORTS, configuration(NOTE_CONFIGURATION, timestep=0),
                "note"),
            "game-schedule": lambda n: self.write(
                n, lint.NOTE, "game", NOTE_EXPORTS,
                configuration(NOTE_CONFIGURATION, scheduleStart=1.5), "note"),
            # readIntKnob: each bound fits an int.
            "int-knob-range": lambda n: self.write(
                n, lint.NOTE, "game",
                exports(NOTE_EXPORTS, knobs={"steps": {"minimum": 1, "maximum": 2**31,
                                                       "default": 8}}),
                NOTE_CONFIGURATION, "note"),
            # The hfa interpreter: one channel, a languages map, a silence label that is a class.
            "hfa-channels": lambda n: self.write_align(n, exports=exports(ALIGN_EXPORTS,
                                                                           channelCount=2)),
            "hfa-no-languages": lambda n: self.write_align(
                n, configuration=configuration(ALIGN_CONFIGURATION, languages=None)),
            "hfa-empty-languages": lambda n: self.write_align(
                n, configuration=configuration(ALIGN_CONFIGURATION, languages={})),
            "hfa-no-silence": lambda n: self.write_align(n, exports=exports(ALIGN_EXPORTS,
                                                                             silenceLabel=None)),
            "hfa-unclassed-silence": lambda n: self.write_align(
                n, exports=exports(ALIGN_EXPORTS, silenceLabel="SIL"),
                vocab=dict(vocab, silent_phonemes=["SP", "AP", "EP", "SIL"])),
            # The hfa interpreter reads these out of the model's own files.
            "hfa-no-hop": lambda n: self.write_align(
                n, config_json={"mel_spec_config": {"sample_rate": 44100}}),
            "hfa-zero-hop": lambda n: self.write_align(
                n, config_json={"mel_spec_config": {"sample_rate": 44100, "hop_size": 0}}),
            "hfa-no-vocab": lambda n: self.write_align(
                n, vocab={k: v for k, v in vocab.items() if k != "vocab"}),
            "hfa-no-silent": lambda n: self.write_align(
                n, vocab={k: v for k, v in vocab.items() if k != "silent_phonemes"}),
            "hfa-no-non-lexical": lambda n: self.write_align(
                n, vocab={k: v for k, v in vocab.items() if k != "non_lexical_phonemes"}),
            # synthrt's loader: the package id and version grammars.
            "package-id": lambda n: self.write(
                n, lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"},
                desc={"id": "test/bad id"}),
            "version-leading-zero": lambda n: self.write(
                n, lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"},
                desc={"version": "1.02.0.0"}),
            "version-five-parts": lambda n: self.write(
                n, lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"},
                desc={"version": "1.0.0.0.0"}),
            "version-not-an-int": lambda n: self.write(
                n, lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"},
                desc={"version": "3000000000.0"}),
        }
        # The lint states the channel rule in the sentence the variant's interpreter gives, so the
        # copies are pinned together here: the three channel cases must report that wording.
        channel_messages = {
            "rmvpe-channels": "the rmvpe variant feeds its model one channel, and the exports "
                              "declare 2",
            "game-channels": "the game variant feeds its models one channel, and the exports "
                             "declare 2",
            "hfa-channels": "the hfa variant feeds its model one channel, and the exports "
                            "declare 2",
        }
        for name, build in cases.items():
            with self.subTest(name):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    report = self.check(build(name))
                self.assertGreater(report.errors, 0)
                if name in channel_messages:
                    self.assertIn(channel_messages[name], output.getvalue(), name)

        # The analyzer's interpreter rejects import options, because no Level 1 contract defines
        # any.
        package = self.write("importing", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"})
        importing = package / "inferences" / "importer"
        importing.mkdir()
        (importing / "inference.json").write_bytes(json.dumps({
            "interface": lint.F0, "level": 1, "variant": "rmvpe", "name": "importer",
            "exports": F0_EXPORTS, "configuration": {"model": "../f0/m.onnx"},
            "imports": [{"role": "analysis/reference", "ref": ":inference/f0",
                         "options": {"depth": 2}}],
        }).encode("utf-8"))
        manifest = json.loads((package / "desc.json").read_text(encoding="utf-8"))
        manifest["contributions"]["inference"].append(
            {"id": "importer", "path": "./inferences/importer/inference.json"})
        (package / "desc.json").write_bytes(json.dumps(manifest).encode("utf-8"))
        self.assertGreater(self.check(package).errors, 0)

    def test_what_the_loader_accepts_the_lint_accepts(self):
        # The readers normalize \ to /, synthrt accepts a version of fewer than four numbers, and
        # the aligner loads dictionaries only for the languages the exports declare.
        backslashed = self.write("backslash", lint.F0, "rmvpe", F0_EXPORTS,
                                 {"model": "..\\f0\\m.onnx"})
        (backslashed / "inferences" / "f0" / "m.onnx").write_bytes(b"\0")
        short = self.write("short", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"},
                           desc={"version": "1.0", "compatVersion": "1.0"})
        configuration = copy.deepcopy(ALIGN_CONFIGURATION)
        configuration["languages"]["eng"] = "en"
        mapped = self.write_align("mapped", configuration=configuration)
        for package in (backslashed, short, mapped):
            report = self.check(package)
            self.assertEqual(report.errors, 0, package.name)

    def test_the_key_tables_mirror_the_schemas(self):
        # The keys of each contract are stated in docs/schemas, in the C++ readers and in the lint.
        # The schemas are the published definitions, so the lint tables are verified against them.
        kinds = {"#/$defs/knob": "knob", "#/$defs/flagKnob": "flag", "#/$defs/intKnob": "intKnob"}
        for interface, name in ((lint.F0, "f0"), (lint.NOTE, "note"), (lint.ALIGN, "align")):
            schema = json.loads((HERE.parent / "docs" / "schemas" /
                                 f"{name}-1-exports.schema.json").read_text(encoding="utf-8"))
            keys = lint.EXPORTS_KEYS[interface]
            self.assertEqual(keys["required"], set(schema["required"]), name)
            self.assertEqual(keys["required"] | keys["optional"], set(schema["properties"]), name)
            knobs = schema["properties"]["knobs"]["properties"]
            self.assertEqual(keys["knobs"], {knob: kinds[value["$ref"]]
                                             for knob, value in knobs.items()}, name)
            self.assertEqual(schema["properties"]["sampleRate"]["maximum"], lint.INT_MAX, name)
            if interface == lint.ALIGN:
                self.assertEqual(lint.ALIGN_LANGUAGE_KEYS,
                                 set(schema["$defs"]["language"]["properties"]))

    def test_the_readers_whitelist_the_same_keys(self):
        # A reader rejects every key it does not know before it reads any of them, so its inline
        # whitelist is a third statement of a contract's keys, and the only one that nothing
        # compared until now: the schemas state the keys and the lint tables restate them, and those
        # two are already checked against each other. Each whitelist is an initializer list inside
        # its call, so it is read out of the source by the message the call passes -- an anchor that
        # names the list and survives rewrites of the surrounding code.
        for interface, stem in ((lint.F0, "F0"), (lint.NOTE, "Note"), (lint.ALIGN, "Align")):
            source = (HERE.parent / "src" / "lib" / "Api" / stem / "1" /
                      f"{stem}ApiL1.cpp").read_text(encoding="utf-8")
            keys = lint.EXPORTS_KEYS[interface]
            self.assertEqual(self.reader_whitelist(source, f'"the {stem} exports"'),
                             keys["required"] | keys["optional"], stem)
            self.assertEqual(self.reader_whitelist(source, f'"the {stem} knobs"'),
                             set(keys["knobs"]), stem)

    def reader_whitelist(self, source: str, anchor: str) -> set:
        """Returns the keys of the one whitelist that *anchor* names in a reader source.

        The list stands between the call and its message, so everything quoted in that span is the
        list. Requiring exactly one occurrence makes a silent failure impossible: a rewrite that
        drops the anchor, or guards the same list twice, fails here instead of comparing nothing.
        """
        occurrences = source.count(anchor)
        if occurrences != 1:
            raise AssertionError(f"{anchor}: expected exactly one whitelist, found {occurrences}")
        end = source.index(anchor)
        start = source.rindex("rejectUnknownKeys(", 0, end)
        return set(re.findall(r'"([^"]+)"', source[start:end]))

    def test_the_import_options_schemas_reject_every_key(self):
        # An analysis contract defines no import options, so the only valid options object is the
        # empty one, and the reader of each contract rejects any other object. The schemas are the
        # published statement of that, so they are checked here rather than left to drift.
        for name in ("f0", "note", "align"):
            schema = json.loads((HERE.parent / "docs" / "schemas" /
                                 f"{name}-1-import-options.schema.json").read_text(encoding="utf-8"))
            self.assertEqual(schema["type"], "object", name)
            self.assertEqual(schema["properties"], {}, name)
            self.assertFalse(schema["additionalProperties"], name)

    def test_every_available_declaration_passes(self):
        # The declarations are no longer tracked: they travel with the model release, so a clean
        # checkout has no packages/ tree at all and an empty set is skipped rather than failed.
        # Where the tree is present it is checked as a whole, one directory per variant, so an
        # edited declaration is caught before a package is assembled from it. The set is what is on
        # disk, not a list of variants kept here.
        root = HERE.parent / "packages"
        descriptors = sorted(root.glob("*/desc.json")) if root.is_dir() else []
        if not descriptors:
            self.skipTest(f"no declarations under {root}: they are no longer tracked, they travel "
                          "with the model release")
        for descriptor in descriptors:
            report = lint.Report()
            lint.check_package(descriptor.parent, report, declarations_only=True)
            self.assertEqual((report.errors, report.warnings), (0, 0), descriptor.parent.name)

    def test_a_package_declares_the_version_of_its_project(self):
        # The library and its packages are released together, so a package that names another
        # version than the project it is built from ships a number no loader would refuse and no
        # reader would question. The fixture carries a project of its own to pin both verdicts.
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.19)\n\nproject(otter\n    VERSION 1.0.0.0\n"
            "    LANGUAGES CXX\n)\n", encoding="utf-8")
        matching = self.write("matching", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"})
        self.assertEqual(self.check(matching, require_project=True).errors, 0,
                         "the project's own version passes")
        stale = self.write("stale", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"},
                           desc={"version": "0.9.0.0", "compatVersion": "0.9.0.0"})
        self.assertEqual(self.check(stale, require_project=True).errors, 1,
                         "another version is reported")

    def test_a_package_with_no_project_above_it_is_an_error(self):
        # A package that is copied out of the tree it was built in has no project above it, and its
        # version is then compared with nothing. Silence is how such a package ships a version no
        # reader would question, so the lint reports the absence unless the caller says otherwise.
        package = self.write("orphan", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"})
        self.assertIsNone(lint.projectversion.find(self.root), "the fixture declares no project")

        report = lint.Report()
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            lint.check_package(package, report)
        self.assertEqual((report.errors, report.warnings), (1, 0))
        self.assertIn("no CMakeLists.txt", output.getvalue())

        # A caller that checks a package outside its tree either hears nothing or hears a warning,
        # and neither affects the exit status.
        self.assertEqual(self.check(package, require_project=False).errors, 0)
        report = lint.Report()
        lint.check_package(package, report, require_project=False, warn_without_project=True)
        self.assertEqual((report.errors, report.warnings), (0, 1))


class ProjectDeclaration(unittest.TestCase):
    """The project version, which the lint, the packager and the release tag all read from one
    place."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="otter-project-"))

    def tearDown(self):
        shutil.rmtree(self.root)

    def write_project(self, directory, text):
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "CMakeLists.txt").write_text(text, encoding="utf-8")
        return directory

    def test_the_project_call_is_read_in_the_forms_cmake_accepts(self):
        cases = {
            "project(otter VERSION 1.0.0.0)": "1.0.0.0",
            "project (otter VERSION 1.0.0.0)": "1.0.0.0",
            "PROJECT(otter VERSION 1.0.0.0)": "1.0.0.0",
            "Project(otter VERSION 1.0.0.0)": "1.0.0.0",
            'project("otter" VERSION 1.0.0.0)': "1.0.0.0",
            "project(otter-lite+ext VERSION 2.3.4.5)": "2.3.4.5",
            "cmake_minimum_required(VERSION 3.19)\n\nproject(otter\n    VERSION 0.1.0.0\n"
            "    LANGUAGES CXX\n)\n": "0.1.0.0",
        }
        for text, version in cases.items():
            with self.subTest(text):
                self.assertEqual(lint.projectversion.declare(text), version)

    def test_a_commented_out_project_call_is_not_a_declaration(self):
        commented = "# project(otter VERSION 9.9.9.9)\n"
        self.assertIsNone(lint.projectversion.declare(commented))
        bracketed = "#[[\nproject(otter VERSION 9.9.9.9)\n]]\n"
        self.assertIsNone(lint.projectversion.declare(bracketed))
        # A commented example above the real call does not decide the version.
        self.assertEqual(lint.projectversion.declare(commented + "project(otter VERSION 1.0.0.0)"),
                         "1.0.0.0")
        self.assertEqual(lint.projectversion.declare(bracketed + "project(otter VERSION 1.0.0.0)"),
                         "1.0.0.0")

    def test_a_mention_of_the_command_is_not_a_declaration(self):
        # The command is read at the start of a line, so a mention of it inside another command
        # does not decide the version of the project that contains it.
        self.assertIsNone(lint.projectversion.declare('message("project(otter VERSION 5.5.5.5)")'))
        self.assertEqual(lint.projectversion.declare("    project(otter VERSION 1.0.0.0)\n"),
                         "1.0.0.0")

    def test_the_nearest_project_declares_the_version(self):
        outer = self.write_project(self.root, "project(otter VERSION 1.0.0.0)\n")
        inner = self.write_project(self.root / "inner", "PROJECT(inner VERSION 2.0.0.0)\n")
        self.assertEqual(lint.projectversion.find(self.root / "package"), (outer, "1.0.0.0"))
        self.assertEqual(lint.projectversion.find(inner), (inner, "2.0.0.0"))
        self.assertEqual(lint.projectversion.find(inner / "package"), (inner, "2.0.0.0"))

        # A CMakeLists.txt that declares no version is passed over for the next one above it.
        versionless = self.write_project(self.root / "versionless",
                                         "cmake_minimum_required(VERSION 3.19)\n")
        self.assertEqual(lint.projectversion.find(versionless / "package"), (outer, "1.0.0.0"))

        # A tree with no project above it has no version to return.
        empty = Path(tempfile.mkdtemp(prefix="otter-noproject-"))
        self.addCleanup(shutil.rmtree, empty)
        self.assertIsNone(lint.projectversion.find(empty))

    def test_the_release_tag_drops_the_trailing_zero_components(self):
        # The tag is the name a release is published under, and the documentation uses it, so the
        # rule that produces it is stated once here rather than in prose.
        self.assertEqual(lint.projectversion.release_tag("0.1.0.0"), "models-v0.1")
        self.assertEqual(lint.projectversion.release_tag("0.3.0.0"), "models-v0.3")
        self.assertEqual(lint.projectversion.release_tag("1.0.0.0"), "models-v1")
        self.assertEqual(lint.projectversion.release_tag("0.1.2.0"), "models-v0.1.2")
        self.assertEqual(lint.projectversion.release_tag("0.0.0.0"), "models-v0",
                         "an all-zero version keeps one component rather than losing its number")


if __name__ == "__main__":
    unittest.main()
