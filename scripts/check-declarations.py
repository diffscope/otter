#!/usr/bin/env python3
"""Lints analysis packages before they are published.

The loader already refuses a declaration it cannot read, so this deliberately does not repeat it.
What it checks is what the loader cannot: whether the files a declaration points at are actually
there, whether the version discipline a package needs in order to be upgradable is in place, and
whether a declaration says something twice in two places that can then disagree.

    python3 scripts/check-declarations.py <package directory>...

Exits non-zero when anything is reported as an error. Warnings do not fail the run.
"""

import argparse
import json
import sys
from pathlib import Path

CATEGORY = "analysis"

# Which configuration keys of each variant name a file, and which of those may be absent.
MODEL_KEYS = {
    ("org.openvpi.analysis.F0", "rmvpe"): {"required": ["model"], "optional": []},
    ("org.openvpi.analysis.Note", "game"): {
        "required": ["encoder", "segmenter", "estimator", "boundaryToDuration"],
        # The alignment model. A package without it is usable, and its declaration says so
        # through exports, so its absence is a fact rather than a fault.
        "optional": ["durationToBoundary"],
    },
}

KNOWN_INTERFACES = {
    "org.openvpi.analysis.F0",
    "org.openvpi.analysis.Note",
    # Reserved. A package declaring one of these is ahead of any implementation, which is worth
    # saying out loud rather than reporting as an unknown contract.
    "org.openvpi.analysis.Align",
    "org.openvpi.analysis.Transcribe",
}

IMPLEMENTED_INTERFACES = {"org.openvpi.analysis.F0", "org.openvpi.analysis.Note"}


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


def four_part(version: str) -> bool:
    parts = version.split(".")
    return len(parts) == 4 and all(part.isdigit() for part in parts)


def order(left: str, right: str) -> int:
    """Compares two four part versions. Negative when left is older."""
    a = [int(part) for part in left.split(".")]
    b = [int(part) for part in right.split(".")]
    return (a > b) - (a < b)


def check_declaration(path: Path, report: Report) -> None:
    where = str(path)
    try:
        declaration = json.loads(path.read_text(encoding="utf-8"))
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

    # Both shipped variants derive exports from configuration, so a declaration stating exports of
    # its own has two sources for one fact, and a host that reads the wrong one prepares audio the
    # model refuses.
    exports = declaration.get("exports")
    if exports not in (None, {}):
        report.error(where, "these variants derive exports from configuration; remove the exports")

    configuration = declaration.get("configuration")
    if not isinstance(configuration, dict):
        report.error(where, "needs a configuration object")
        return

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

    if interface == "org.openvpi.analysis.Note":
        languages = configuration.get("languages")
        if languages is not None and not isinstance(languages, dict):
            report.error(where, "languages must map identifiers to the model's numbering")
        default = configuration.get("defaultLanguage")
        if default is not None and isinstance(languages, dict) and default not in languages:
            report.error(where, f"the default language {default} is not one this model declares")
        if "durationToBoundary" not in configuration:
            report.warn(where, "no alignment model; transcription cannot be conditioned on a score")


def check_package(root: Path, report: Report) -> None:
    where = str(root)
    desc = root / "desc.json"
    if not desc.is_file():
        report.error(where, "has no desc.json")
        return
    try:
        manifest = json.loads(desc.read_text(encoding="utf-8"))
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
