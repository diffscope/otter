"""Tests for make-package.py, the package assembler.

A release identifies each archive by its SHA512, so the same inputs must produce the same bytes,
and a model must never be packaged under the path of another file. Run with
python -m unittest discover -s scripts.
"""

import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load_packager():
    spec = importlib.util.spec_from_file_location("make_package", HERE / "make-package.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


packager = load_packager()

F0 = "org.openvpi.otter.inference.F0"
ALIGN = "org.openvpi.otter.inference.Align"


class Assembly(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="otter-package-"))
        self.declarations = self.root / "declarations"
        self.models = self.root / "models"
        self.models.mkdir()
        self.saved = packager.DECLARATIONS
        packager.DECLARATIONS = self.declarations

    def tearDown(self):
        packager.DECLARATIONS = self.saved
        shutil.rmtree(self.root)

    def declare(self, variant, declaration):
        package = self.declarations / variant
        directory = package / "inferences" / "x"
        directory.mkdir(parents=True)
        (package / "desc.json").write_text(json.dumps({
            "$version": "1.0", "id": f"test/{variant}", "version": "1.0.0.0",
            "compatVersion": "1.0.0.0", "runtimeLevel": 1,
            "contributions": {"inference": [{"id": "x", "path": "./inferences/x/inference.json"}]},
        }), encoding="utf-8")
        # A comment, which the JSON profile of the specification allows and a plain JSON reader
        # rejects.
        (directory / "inference.json").write_text(
            "// the declaration\n" + json.dumps(declaration), encoding="utf-8")

    def test_the_same_inputs_give_the_same_bytes(self):
        self.declare("rmvpe", {
            "interface": F0, "level": 1, "variant": "rmvpe", "name": "r",
            "exports": {"sampleRate": 16000, "interval": 0.01},
            "configuration": {"model": "../../rmvpe.onnx"},
        })
        (self.models / "rmvpe.onnx").write_bytes(bytes(range(256)) * 5000)
        archives = []
        for run in ("first", "second"):
            output = self.root / run
            output.mkdir()
            directory, _, version, _, shipped = packager.assemble("rmvpe", self.models, output,
                                                                  None)
            self.assertEqual(shipped, [Path("rmvpe.onnx")])
            archives.append(packager.archive(directory, version, output).read_bytes())
        self.assertEqual(archives[0], archives[1])

    def test_a_shared_file_name_is_refused(self):
        # Two dictionaries with the same name in different directories: a lookup by name would
        # package one file under the path of the other.
        self.declare("hfa", {
            "interface": ALIGN, "level": 1, "variant": "hfa", "name": "h",
            "exports": {"sampleRate": 44100, "silenceLabel": "SP"},
            "configuration": {"model": "../../model.onnx", "config": "../../config.json",
                              "vocab": "../../vocab.json", "languages": {"zxx": "zx"}},
        })
        for name in ("model.onnx", "config.json"):
            (self.models / name).write_bytes(b"{}")
        (self.models / "vocab.json").write_text(json.dumps(
            {"dictionaries": {"zx": "a/dict.txt", "yy": "b/dict.txt"}}), encoding="utf-8")
        (self.models / "dict.txt").write_text("a\ta\n", encoding="utf-8")
        output = self.root / "out"
        output.mkdir()
        with self.assertRaises(SystemExit):
            packager.assemble("hfa", self.models, output, None)

        # With the package layout, each file is found at its own path.
        for directory in ("a", "b"):
            (self.models / directory).mkdir()
            (self.models / directory / "dict.txt").write_text(directory, encoding="utf-8")
        package, _, _, _, shipped = packager.assemble("hfa", self.models, output, None)
        self.assertIn(Path("a/dict.txt"), shipped)
        self.assertIn(Path("b/dict.txt"), shipped)
        self.assertEqual((package / "a" / "dict.txt").read_text(encoding="utf-8"), "a")
        self.assertEqual((package / "b" / "dict.txt").read_text(encoding="utf-8"), "b")


if __name__ == "__main__":
    unittest.main()
