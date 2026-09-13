#!/usr/bin/env python3
"""Lints analysis packages before they are published.

The loader already refuses a declaration it cannot read, so this deliberately does not repeat it.
What it checks is what the loader cannot: whether the files a declaration points at are actually
there, whether the version discipline a package needs in order to be upgradable is in place, and
whether the two blocks of a declaration agree with each other. The exports block is the contract's
and is checked against docs/schemas; the configuration block is the variant's and is checked
against what the shipped variants read.

    python3 scripts/check-declarations.py <package directory>...

Exits non-zero when anything is reported as an error. Warnings do not fail the run.
"""

import argparse
import json
import re
import pathlib
import sys
from pathlib import Path


def load_json(path):
    """Reads a JSON file the way the loader does (spec 2.4 JSON profile).

    A UTF-8 BOM is allowed, `//` and `/* */` comments are allowed outside strings, and a
    repeated key makes the whole document invalid, since which value wins would otherwise depend
    on the parser.
    """
    text = pathlib.Path(path).read_text(encoding="utf-8-sig")
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                raise ValueError(f"{path}: unterminated comment")
            i = j + 2
        else:
            out.append(c)
            i += 1

    def no_duplicates(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"{path}: key {key!r} appears twice")
            result[key] = value
        return result

    return json.loads("".join(out), object_pairs_hook=no_duplicates)

CATEGORY = "analysis"

F0 = "org.openvpi.otter.analysis.F0"
NOTE = "org.openvpi.otter.analysis.Note"

# Which configuration keys of each variant name a file, and which of those may be absent.
MODEL_KEYS = {
    (F0, "rmvpe"): {"required": ["model"], "optional": []},
    (NOTE, "game"): {
        "required": ["encoder", "segmenter", "estimator", "boundaryToDuration"],
        # The alignment model. A package without it is usable, and its declaration says so
        # through exports, so its absence is a fact rather than a fault.
        "optional": ["durationToBoundary"],
    },
}

# Every key a variant's configuration may carry. A key outside this set is refused by the provider
# at load, so it is refused here too, before the package ships.
CONFIGURATION_KEYS = {
    (F0, "rmvpe"): {"model"},
    (NOTE, "game"): {
        "encoder", "segmenter", "estimator", "boundaryToDuration", "durationToBoundary",
        "timestep", "languages", "defaultLanguage", "scheduleStart",
    },
}

# The exports keys each contract defines, mirroring docs/schemas/<contract>-1-exports.schema.json.
EXPORTS_KEYS = {
    F0: {
        "required": {"sampleRate", "interval"},
        "optional": {"channelCount", "maxSegmentDuration", "knobs"},
        "knobs": {"voicingThreshold": "knob", "interpolateUnvoiced": "flag"},
    },
    NOTE: {
        "required": {"sampleRate"},
        "optional": {"channelCount", "maxSegmentDuration", "languages", "supportsKnownNotes",
                     "knobs"},
        "knobs": {"boundaryThreshold": "knob", "boundaryRadius": "knob", "noteThreshold": "knob",
                  "notePresenceCutoff": "knob", "steps": "intKnob"},
    },
}

KNOWN_INTERFACES = {
    F0,
    NOTE,
    # Reserved. A package declaring one of these is ahead of any implementation, which is worth
    # saying out loud rather than reporting as an unknown contract.
    "org.openvpi.otter.analysis.Align",
    "org.openvpi.otter.analysis.Transcribe",
}

IMPLEMENTED_INTERFACES = {F0, NOTE}


class Report:
    def __init__(self) -> None:
        self.errors = 0
        self.warnings = 0

    def error(self, where: str, message: str) -> None:
        print(f"error: {where}: {message}")
        self.errors += 1

    def warn(self, where: str, message: str) -> None:
        print(f"warning: {where}: {message}")
        self.warnings += 1


VERSION = re.compile(r"(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){0,3}")


def four_part(version: str) -> bool:
    """The version grammar the specification gives: one to four decimal components, no leading
    zeros. Kept under its old name because the callers read as "is this a well formed version"."""
    return isinstance(version, str) and VERSION.fullmatch(version) is not None


def order(left: str, right: str) -> int:
    """Compares two versions. Negative when left is older; missing components count as zero."""
    a = [int(part) for part in left.split(".")] + [0] * 4
    b = [int(part) for part in right.split(".")] + [0] * 4
    a, b = a[:4], b[:4]
    return (a > b) - (a < b)


def check_knob(where: str, name: str, kind: str, value, report: Report) -> None:
    if not isinstance(value, dict):
        report.error(where, f"the knob {name} must be an object")
        return
    if kind == "flag":
        if set(value) != {"default"} or not isinstance(value["default"], bool):
            report.error(where, f"the knob {name} takes exactly one boolean default")
        return
    if set(value) != {"minimum", "maximum", "default"}:
        report.error(where, f"the knob {name} needs minimum, maximum and default, nothing else")
        return
    numeric = int if kind == "intKnob" else (int, float)
    for key, item in value.items():
        if isinstance(item, bool) or not isinstance(item, numeric):
            report.error(where, f"the knob {name}.{key} must be a{'n integer' if kind == 'intKnob' else ' number'}")
            return
    if not value["minimum"] <= value["default"] <= value["maximum"]:
        report.error(where, f"the knob {name} must have minimum <= default <= maximum")


def check_exports(where: str, interface: str, exports, report: Report) -> dict:
    """Checks the exports block against the contract and returns it, or an empty dict."""
    if not isinstance(exports, dict):
        report.error(where, "needs an exports object; the audio format and the knobs live there")
        return {}
    keys = EXPORTS_KEYS[interface]
    for key in keys["required"]:
        if key not in exports:
            report.error(where, f"the exports need a {key}")
    for key in exports:
        if key not in keys["required"] | keys["optional"]:
            report.error(where, f"{key} is not a key the {interface} exports define")
    for key in ("sampleRate", "channelCount"):
        value = exports.get(key)
        if value is not None and (isinstance(value, bool) or not isinstance(value, int) or value < 1):
            report.error(where, f"{key} must be a positive integer")
    for key in ("interval", "maxSegmentDuration"):
        value = exports.get(key)
        if value is not None and (isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0):
            report.error(where, f"{key} must be a number greater than zero")
    if "supportsKnownNotes" in exports and not isinstance(exports["supportsKnownNotes"], bool):
        report.error(where, "supportsKnownNotes must be a boolean")
    languages = exports.get("languages")
    if languages is not None:
        if not isinstance(languages, list) or not all(isinstance(x, str) and x for x in languages):
            report.error(where, "languages must be a list of non-empty identifiers")
        elif len(set(languages)) != len(languages):
            report.error(where, "languages must not repeat")
    knobs = exports.get("knobs")
    if knobs is not None:
        if not isinstance(knobs, dict):
            report.error(where, "knobs must be an object")
        else:
            for name, value in knobs.items():
                kind = keys["knobs"].get(name)
                if kind is None:
                    report.error(where, f"{name} is not a knob the {interface} contract defines")
                    continue
                check_knob(where, name, kind, value, report)
    return exports


def check_declaration(path: Path, report: Report) -> None:
    where = str(path)
    try:
        declaration = load_json(path)
    except (OSError, json.JSONDecodeError) as problem:
        report.error(where, f"cannot be read: {problem}")
        return

    interface = declaration.get("interface")
    variant = declaration.get("variant")
    level = declaration.get("level")
    if not isinstance(interface, str) or not isinstance(variant, str):
        report.error(where, "needs a string interface and variant")
        return
    if level != 1:
        report.error(where, f"level {level!r} is not a level this repository implements")
        return

    if interface not in KNOWN_INTERFACES:
        report.warn(where, f"{interface} is not a contract this repository defines")
        return
    if interface not in IMPLEMENTED_INTERFACES:
        report.warn(where, f"{interface} is reserved and has no implementation yet")
        return

    keys = MODEL_KEYS.get((interface, variant))
    if keys is None:
        report.warn(where, f"no variant named {variant} implements {interface} here")
        return

    exports = check_exports(where, interface, declaration.get("exports"), report)

    configuration = declaration.get("configuration")
    if not isinstance(configuration, dict):
        report.error(where, "needs a configuration object")
        return
    for key in configuration:
        if key not in CONFIGURATION_KEYS[(interface, variant)]:
            report.error(where, f"{key} is not a key the {variant} configuration reads; "
                                f"the audio format and the knobs belong in exports")

    for key in keys["required"]:
        if key not in configuration:
            report.error(where, f"the configuration needs a {key}")
    for key in keys["required"] + keys["optional"]:
        value = configuration.get(key)
        if value is None:
            continue
        if not isinstance(value, str):
            report.error(where, f"{key} must be a string holding a path")
            continue
        target = (path.parent / value).resolve()
        if not target.is_file():
            report.error(where, f"{key} points at a file that is not there: {value}")
        elif target.stat().st_size == 0:
            report.error(where, f"{key} points at an empty file: {value}")

    if interface == NOTE:
        numbering = configuration.get("languages")
        if numbering is not None and not isinstance(numbering, dict):
            report.error(where, "languages must map identifiers to the model's numbering")
            numbering = {}
        default = configuration.get("defaultLanguage")
        if default is not None and isinstance(numbering, dict) and default not in numbering:
            report.error(where, f"the default language {default} is not one this model numbers")
        # The two blocks must agree: every language the exports promise needs a number, and an
        # alignment path promised needs the model that performs it. The provider refuses the same
        # two things at load; catching them here is what lets a package be fixed before it ships.
        for language in exports.get("languages", []) if isinstance(exports.get("languages"), list) else []:
            if isinstance(numbering, dict) and language not in numbering:
                report.error(where, f"the exports list {language} but the configuration gives it no numbering")
        if exports.get("supportsKnownNotes") and "durationToBoundary" not in configuration:
            report.error(where, "the exports declare supportsKnownNotes but there is no durationToBoundary model")
        if "durationToBoundary" in configuration and not exports.get("supportsKnownNotes"):
            report.warn(where, "an alignment model is shipped but the exports do not declare supportsKnownNotes, so no host will use it")
        if "durationToBoundary" not in configuration:
            report.warn(where, "no alignment model; transcription cannot be conditioned on a score")


def check_package(root: Path, report: Report) -> None:
    where = str(root)
    desc = root / "desc.json"
    if not desc.is_file():
        report.error(where, "has no desc.json")
        return
    try:
        manifest = load_json(desc)
    except (OSError, json.JSONDecodeError) as problem:
        report.error(str(desc), f"cannot be read: {problem}")
        return

    version = manifest.get("version")
    if not isinstance(version, str) or not four_part(version):
        report.error(str(desc), "needs a four part version")
    else:
        compat = manifest.get("compatVersion")
        if compat is None:
            # A package without a floor claims compatibility back to nothing, so a host with a
            # dependency on it cannot accept a newer build. Cheap to state, expensive to retrofit
            # once packages are in the wild.
            report.warn(str(desc), "declares no compatVersion; upgrades cannot be accepted")
        elif not isinstance(compat, str) or not four_part(compat):
            report.error(str(desc), "compatVersion must be a four part version")
        elif order(compat, version) > 0:
            report.error(str(desc), f"compatVersion {compat} is newer than version {version}")

    entries = manifest.get("contributions", {}).get(CATEGORY)
    if not entries:
        report.error(str(desc), "declares no analysis contributions")
        return
    seen = set()
    for entry in entries:
        identifier = entry.get("id")
        if not identifier:
            report.error(str(desc), "a contribution entry has no id")
            continue
        if identifier in seen:
            report.error(str(desc), f"two contributions share the id {identifier}")
        seen.add(identifier)
        relative = entry.get("path")
        if not relative:
            report.error(str(desc), f"{identifier} names no declaration")
            continue
        declaration = (root / relative).resolve()
        if not declaration.is_file():
            report.error(str(desc), f"{identifier} points at a declaration that is not there")
            continue
        check_declaration(declaration, report)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packages", nargs="+", type=Path)
    args = parser.parse_args()

    report = Report()
    for root in args.packages:
        if not root.is_dir():
            report.error(str(root), "is not a directory")
            continue
        check_package(root, report)

    checked = len(args.packages)
    print(f"checked {checked} package(s): {report.errors} error(s), {report.warnings} warning(s)")
    return 1 if report.errors else 0


if __name__ == "__main__":
    sys.exit(main())
