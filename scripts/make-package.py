#!/usr/bin/env python3
"""Assembles the declaration of one variant and its models into an installable package.

The repository holds declarations without model weights (see ``packages/.gitignore``). Producing
the archive that a host installs takes four steps: copying the declaration, placing the model files
at the paths the declaration specifies, running the declaration lint on the result, and recording
the output. The script performs these steps so that every package is assembled identically.

Usage:

    python3 scripts/make-package.py --variant hfa --models /path/to/hubertfa-v0.0.7 \\
        --manifest build/packages/manifest.json --bundle 0.3.0.0

The archive is deterministic: the entries are sorted, their timestamps are fixed, and the
compression level is fixed, so the same inputs produce the same bytes and the same SHA512. Each
model file is looked up under ``--models`` first at its path inside the package and then by file
name, so an unmodified download directory is a valid argument. A file name shared by two files of
the package is rejected instead of being resolved to one of them.

The declarations are read with the lint's reader (the JSON profile of spec 2.4), and the files to
package are the values of the configuration keys that the lint classifies as file paths. The
packager and the lint therefore apply the same rules to both.

The script performs no network access and publishes nothing: it writes the archive and the manifest
fragment under ``--output``. Publication is a separate step.
"""

import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent
DECLARATIONS = REPOSITORY / "packages"


def load_lint():
    """Imports check-declarations.py, whose file name is not a module name."""
    spec = importlib.util.spec_from_file_location(
        "check_declarations", REPOSITORY / "scripts" / "check-declarations.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


LINT = load_lint()

# Chunk size for copying a file into the archive, so that a 400 MB model is never held in memory
# in full.
CHUNK = 1 << 20

# Timestamp of every entry, so that two runs produce identical archives. Zip stores local time in
# MS-DOS format, which cannot represent dates before 1980; 1980-01-01 is the conventional minimum.
FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def digest(path: Path, algorithm: str) -> str:
    """Returns the hex digest of a file, reading it in chunks to bound memory use."""
    hasher = hashlib.new(algorithm)
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(CHUNK), b""):
            hasher.update(block)
    return hasher.hexdigest()


def declaration_files(variant: Path) -> list[tuple[str, Path]]:
    """Lists the files the declarations of a variant refer to.

    :returns: a list of (key, path in the declaration tree) pairs. A file is the value of a
              configuration key that the lint classifies as a file path for the variant. The other
              configuration values are language codes, numbers and names, and the lint rejects
              every unknown key.
    """
    wanted = []
    for declaration in sorted(variant.glob("inferences/*/inference.json")):
        document = LINT.load_json(declaration)
        configuration = document.get("configuration", {})
        if not isinstance(configuration, dict):
            raise SystemExit(f"{declaration}: configuration is not an object")
        keys = LINT.MODEL_KEYS.get((document.get("interface"), document.get("variant")))
        if keys is None:
            raise SystemExit(f"{declaration}: the declaration matches no shipped variant")
        for key in keys["required"] + keys["optional"]:
            value = configuration.get(key)
            if not isinstance(value, str):
                continue
            # A relative path is resolved against the directory of the declaration that contains
            # it, as the specification requires and the lint checks.
            wanted.append((key, LINT.resolve(declaration.parent, value)))
    return wanted


def companion_files(document: dict, directory: Path) -> list[Path]:
    """Lists the files that a model's own JSON file names relative to its directory.

    A declaration names the models and the files that the provider reads directly. A model's
    vocabulary additionally names the pronunciation dictionaries in its directory, and the provider
    reads that list instead of the declaration. A package assembled from the declaration alone
    would therefore contain a vocabulary that refers to missing dictionaries, which the lint reports
    once per language.
    """
    named = []
    dictionaries = document.get("dictionaries")
    if isinstance(dictionaries, dict):
        for value in dictionaries.values():
            if isinstance(value, str):
                named.append(directory / value.replace("\\", "/"))
    return named


def find_model(models: Path, inside: Path, names: dict) -> Path | None:
    """Finds, under models, the file that belongs at the package path inside.

    The path inside the package is tried first, so a model tree with the package layout requires no
    lookup by name. Otherwise the file is looked up by name, which is rejected if two files of the
    package share that name: the lookup would be ambiguous, and a model packaged under the path of
    another file would load and produce wrong results instead of failing.

    :returns: the path of the file, or None if no file is found.
    """
    candidate = models / inside
    if candidate.is_file():
        return candidate
    if len(names.get(inside.name, ())) > 1:
        paths = ", ".join(sorted(str(path) for path in names[inside.name]))
        raise SystemExit(f"{paths} share the name {inside.name}; arrange {models} in the "
                         f"package layout so that each file is found at its own path")
    candidate = models / inside.name
    return candidate if candidate.is_file() else None


def assemble(variant: str, models: Path, output: Path,
             license_file: Path | None) -> tuple[Path, str, str, dict, list[Path]]:
    """Copies the declaration and the files it needs into a package directory.

    :returns: the package directory, the package id, the package version, the package manifest as
              read, and every packaged model file, as paths inside the package.
    """
    source = DECLARATIONS / variant
    desc = LINT.load_json(source / "desc.json")
    identifier = desc["id"]
    version = desc["version"]

    # Root directory of the archive, which the manifest also records. A published package directory
    # is named after the package id with the slash replaced by a dash.
    directory = output / identifier.replace("/", "-")
    if directory.exists():
        shutil.rmtree(directory)
    shutil.copytree(source, directory)

    declared = []
    for key, destination in declaration_files(source):
        inside = destination.relative_to(source)
        if inside not in (path for _, path in declared):
            declared.append((key, inside))

    def ship(inside: Path, what: str, names: dict) -> None:
        candidate = find_model(models, inside, names)
        if candidate is None:
            raise SystemExit(f"{what} names {inside}, which is missing from {models}")
        (directory / inside).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(candidate, directory / inside)
        shipped.append(inside)

    shipped = []
    names = {}
    for _, inside in declared:
        names.setdefault(inside.name, set()).add(inside)
    for key, inside in declared:
        ship(inside, f"the declaration's {key}", names)

    # A model's own JSON file may name further files in its directory, which the provider locates
    # through that JSON file instead of through the declaration.
    companions = []
    for inside in list(shipped):
        if inside.suffix != ".json":
            continue
        for companion in companion_files(LINT.load_json(directory / inside), inside.parent):
            companion = Path(*companion.parts)
            if companion not in shipped and companion not in (c for _, c in companions):
                companions.append((inside, companion))
    for _, companion in companions:
        names.setdefault(companion.name, set()).add(companion)
    for owner, companion in companions:
        ship(companion, str(owner), names)

    if license_file is not None:
        shutil.copy2(license_file, directory / "LICENSE")
    return directory, identifier, version, desc, shipped


def lint(directory: Path) -> None:
    """Runs the declaration lint on an assembled package and forwards its output."""
    result = subprocess.run(
        [sys.executable, str(REPOSITORY / "scripts" / "check-declarations.py"), str(directory)],
        capture_output=True,
        text=True,
    )
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        raise SystemExit(f"the declaration lint rejects {directory}")


def archive(directory: Path, version: str, output: Path) -> Path:
    """Writes the package directory as a zip archive whose entries share one root directory."""
    root = directory.name
    target = output / f"{root}-{version}.zip"
    members = sorted(path for path in directory.rglob("*") if path.is_file())
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for member in members:
            relative = member.relative_to(directory).as_posix()
            info = zipfile.ZipInfo(f"{root}/{relative}", date_time=FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            # The size is set before the entry is opened, as writestr does, so the entry header is
            # identical and the file is streamed in chunks instead of being read in full.
            info.file_size = member.stat().st_size
            with member.open("rb") as source, bundle.open(info, "w") as entry:
                shutil.copyfileobj(source, entry, CHUNK)
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--variant", required=True, help="name of a directory under packages/")
    parser.add_argument("--models", required=True, type=Path,
                        help="directory containing the model files, searched by package path and then by file name")
    parser.add_argument("--output", default=REPOSITORY / "build" / "packages", type=Path,
                        help="output directory (default: build/packages)")
    parser.add_argument("--license", dest="license_file", type=Path,
                        help="file packaged as LICENSE if the model's own archive contains none")
    parser.add_argument("--manifest", type=Path,
                        help="merge the fragment into this manifest, creating it if absent")
    parser.add_argument("--bundle",
                        help="bundleVersion of the manifest; required if the manifest does not exist")
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    directory, identifier, version, desc, shipped = assemble(args.variant, args.models,
                                                             args.output, args.license_file)
    lint(directory)
    target = archive(directory, version, args.output)

    # Each model is listed with its own digest, so that a host can distinguish a repackaged archive
    # from replaced weights without unpacking either archive.
    models = {}
    for inside in sorted(shipped):
        models[inside.as_posix() if inside.name in models else inside.name] = digest(
            directory / inside, "sha512")

    fragment = {
        "id": identifier,
        "file": target.name,
        "sha512": digest(target, "sha512"),
        "version": version,
        "compatVersion": desc["compatVersion"],
        "directory": directory.name,
        "size": target.stat().st_size,
        "models": models,
    }

    if args.manifest is not None:
        if args.manifest.exists():
            manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
            if not manifest.get("bundleVersion"):
                raise SystemExit(f"{args.manifest} contains no bundleVersion")
            # A new release sets the bundle version: a host reads the version of the bundle from
            # the manifest published with the release.
            if args.bundle:
                manifest["bundleVersion"] = args.bundle
        else:
            if not args.bundle:
                raise SystemExit("--bundle is required if the manifest does not exist")
            manifest = {"bundleVersion": args.bundle, "packages": []}
        packages = [entry for entry in manifest["packages"] if entry.get("id") != identifier]
        manifest["packages"] = sorted(packages + [fragment], key=lambda entry: entry["id"])
        args.manifest.write_text(json.dumps(manifest, indent=4) + "\n", encoding="utf-8")

    print(json.dumps(fragment, indent=4))
    print(f"wrote {target}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
