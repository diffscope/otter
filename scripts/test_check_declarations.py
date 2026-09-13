"""Tests for check-declarations.py, the package lint.

The lint is what stands between a declaration and a release, so what it refuses is pinned here:
one good package of each contract must pass clean, and each kind of disagreement between the two
blocks of a declaration must be reported. Run with python -m unittest discover -s scripts.
"""

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


class Packages(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="otter-lint-"))

    def tearDown(self):
        shutil.rmtree(self.root)

    def write(self, name, interface, variant, exports, configuration, contribution="f0"):
        package = self.root / name
        directory = package / "analyzers" / contribution
        directory.mkdir(parents=True)
        (package / "desc.json").write_text(json.dumps({
            "$version": "1.0",
            "id": f"test/{name}",
            "version": "1.0.0.0",
            "compatVersion": "1.0.0.0",
            "runtimeLevel": 1,
            "contributions": {
                "analysis": [{"id": contribution, "path": f"./analyzers/{contribution}/analysis.json"}]
            },
        }), encoding="utf-8")
        (directory / "analysis.json").write_text(json.dumps({
            "interface": interface,
            "level": 1,
            "variant": variant,
            "name": name,
            "exports": exports,
            "configuration": configuration,
        }), encoding="utf-8")
        # Every path a configuration names must exist and be non-empty for the lint to pass it.
        for key, value in configuration.items():
            if isinstance(value, str) and value.endswith(".onnx"):
                (directory / value).write_bytes(b"\0")
        return package

    def check(self, package):
        report = lint.Report()
        lint.check_package(package, report)
        return report

    def test_good_packages_pass_clean(self):
        rmvpe = self.write("rmvpe", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./rmvpe.onnx"})
        game = self.write("game", lint.NOTE, "game", NOTE_EXPORTS, NOTE_CONFIGURATION, "note")
        for package in (rmvpe, game):
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

    def test_a_missing_model_file_is_an_error(self):
        package = self.write("nofile", lint.F0, "rmvpe", F0_EXPORTS, {"model": "./m.onnx"})
        (package / "analyzers" / "f0" / "m.onnx").unlink()
        self.assertGreater(self.check(package).errors, 0)


if __name__ == "__main__":
    unittest.main()
