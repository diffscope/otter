"""Tests for check-declarations.py, the package lint.

The lint is the last check before a declaration is released, so these tests fix its verdicts: a
valid package of each contract must pass without findings, and every kind of inconsistency between
the two blocks of a declaration must be reported. Run with python -m unittest discover -s scripts.
"""

import copy
import importlib.util
import json
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
        (package / "desc.json").write_text(json.dumps(manifest), encoding="utf-8")
        (directory / "inference.json").write_text(json.dumps({
            "interface": interface,
            "level": 1,
            "variant": variant,
            "name": name,
            "exports": exports,
            "configuration": configuration,
        }), encoding="utf-8")
        # The lint requires every path named by a configuration to exist and be non-empty.
        for key, value in configuration.items():
            if isinstance(value, str) and value.endswith(".onnx"):
                (directory / value).write_bytes(b"\0")
        return package

    def check(self, package):
        report = lint.Report()
        lint.check_package(package, report)
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
        for package in (rmvpe, game, hfa):
            report = self.check(package)
            self.assertEqual((report.errors, report.warnings), (0, 0), package.name)

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
        # The declarations in packages/ are kept without their models, and CI checks them in this
        # mode. It must still report what is wrong with a declaration.
        package = self.write("nofiles", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"})
        (package / "inferences" / "f0" / "m.onnx").unlink()
        report = lint.Report()
        lint.check_package(package, report, declarations_only=True)
        self.assertEqual((report.errors, report.warnings), (0, 0))

        hfa = self.write_align("hfanofiles")
        for name in ("model.onnx", "config.json", "vocab.json", "zx_dict.txt"):
            (hfa / name).unlink()
        report = lint.Report()
        lint.check_package(hfa, report, declarations_only=True)
        self.assertEqual((report.errors, report.warnings), (0, 0))

        exports = dict(F0_EXPORTS)
        del exports["sampleRate"]
        broken = self.write("brokennofiles", lint.F0, "rmvpe", exports, {"model": "./m.onnx"})
        report = lint.Report()
        lint.check_package(broken, report, declarations_only=True)
        self.assertGreater(report.errors, 0)

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
        for name, build in cases.items():
            with self.subTest(name):
                self.assertGreater(self.check(build(name)).errors, 0)

        # The analyzer's interpreter rejects import options, because no Level 1 contract defines
        # any.
        package = self.write("importing", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"})
        importing = package / "inferences" / "importer"
        importing.mkdir()
        (importing / "inference.json").write_text(json.dumps({
            "interface": lint.F0, "level": 1, "variant": "rmvpe", "name": "importer",
            "exports": F0_EXPORTS, "configuration": {"model": "../f0/m.onnx"},
            "imports": [{"role": "analysis/reference", "ref": ":inference/f0",
                         "options": {"depth": 2}}],
        }), encoding="utf-8")
        manifest = json.loads((package / "desc.json").read_text(encoding="utf-8"))
        manifest["contributions"]["inference"].append(
            {"id": "importer", "path": "./inferences/importer/inference.json"})
        (package / "desc.json").write_text(json.dumps(manifest), encoding="utf-8")
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

    def test_the_shipped_declarations_pass(self):
        # CI runs the same check over packages/, so that an edited declaration is checked before a
        # package is assembled from it.
        for name in ("rmvpe", "game", "hfa"):
            report = lint.Report()
            lint.check_package(HERE.parent / "packages" / name, report, declarations_only=True)
            self.assertEqual((report.errors, report.warnings), (0, 0), name)


if __name__ == "__main__":
    unittest.main()
