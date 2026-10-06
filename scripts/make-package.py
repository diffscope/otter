#!/usr/bin/env python3
"""Assembles the declaration of one variant and its models into an installable package.

A declaration is not tracked in this repository: it travels with the release that ships the package,
and assembly reads it from a local directory that holds one subdirectory per variant. The model files
it names are not tracked either. Producing the archive that a host installs takes four steps: copying
the declaration, placing the model files at the paths the declaration specifies, running the
declaration lint on the result, and recording the output. The script performs these steps so that
every package is assembled identically.

Usage:

    python3 scripts/make-package.py --variant hfa --models /path/to/hubertfa-v0.0.7 \\
        --manifest build/packages/manifest.json

The archive is deterministic: the entries are sorted by their path inside the archive as a POSIX
string, which orders them the same way on every platform, and their timestamps and creating system
are fixed, so the same inputs order the entries identically and produce the same SHA512 wherever the
archive is assembled. The entries are deflated at the level zlib applies by default, because the
level of an entry written through a ZipInfo object is that default, so the bytes are also identical
only as long as the platform's zlib is the same. The declarations are copied as they are, so their
bytes, line endings included, are part of that hash: whoever prepares them fixes them to one line
ending, or two hosts produce two archives. Each model file is looked up under ``--models`` first at
its path inside the package and then by file name, so an unmodified download directory is a valid
argument. A file name shared by two files of the package is rejected instead of being resolved to
one of them.

The version of a release is the version of the project, so it is not an argument: ``--bundle``
accepts that value or the release tag of it and refuses anything else. The release tag of a version
drops its trailing zero components, which is how 0.1.0.0 is published as ``models-v0.1``, and the
script prints the tag of the release it assembled. The version is read when a package is assembled
rather than at entry, so a tree that holds nothing but these scripts still shows its usage.

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

# The scripts of this directory are not a package, so the directory goes on the path for
# projectversion, which states the project version this script and the lint both read.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import projectversion

SCRIPTS = Path(__file__).resolve().parent
REPOSITORY = SCRIPTS.parent

# Default of --declarations: the directory below the repository root that holds one subdirectory per
# variant. The declarations themselves are not tracked, so the default is a convention for a checkout
# that keeps them there rather than a file this repository carries.
DEFAULT_DECLARATIONS = REPOSITORY / "packages"

# The declaration lint, loaded on first use. It sits beside this script, and loading it reads a
# file, so a run that ends before any declaration is read, such as --help or a rejected argument,
# needs nothing on disk beside the scripts themselves.
LINT = None


def load_lint():
    """Imports check-declarations.py, whose file name is not a module name."""
    global LINT
    if LINT is None:
        path = SCRIPTS / "check-declarations.py"
        try:
            spec = importlib.util.spec_from_file_location("check_declarations", path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
        except (OSError, ImportError, AttributeError) as problem:
            raise SystemExit(f"the declaration lint cannot be read from {path}: {problem}")
        LINT = module
    return LINT

# Chunk size for copying a file into the archive, so that a 400 MB model is never held in memory
# in full.
CHUNK = 1 << 20

# Timestamp of every entry, so that two runs produce identical archives. Zip stores local time in
# MS-DOS format, which cannot represent dates before 1980; 1980-01-01 is the conventional minimum.
FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def digest(path: Path, algorithm: str) -> str:
    """Returns the hex digest of a file, reading it in chunks to bound memory use.

    A directory is hashed as one unit: the names and the contents of its files, walked in the
    order of their POSIX-relative paths, so the digest of the same tree is the same everywhere and
    a replaced file inside it changes the digest of the whole.
    """
    if path.is_dir():
        hasher = hashlib.new(algorithm)
        members = sorted((member for member in path.rglob("*") if member.is_file()),
                         key=lambda member: member.relative_to(path).as_posix())
        for member in members:
            name = member.relative_to(path).as_posix().encode("utf-8")
            hasher.update(len(name).to_bytes(8, "little"))
            hasher.update(name)
            hasher.update(member.stat().st_size.to_bytes(8, "little"))
            with member.open("rb") as handle:
                for block in iter(lambda: handle.read(CHUNK), b""):
                    hasher.update(block)
        return hasher.hexdigest()
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
    lint = load_lint()
    for declaration in sorted(variant.glob("inferences/*/inference.json")):
        document = lint.load_json(declaration)
        configuration = document.get("configuration", {})
        if not isinstance(configuration, dict):
            raise SystemExit(f"{declaration}: configuration is not an object")
        keys = lint.MODEL_KEYS.get((document.get("interface"), document.get("variant")))
        if keys is None:
            raise SystemExit(f"{declaration}: the declaration matches no shipped variant")
        for key in keys["required"] + keys["optional"]:
            value = configuration.get(key)
            if not isinstance(value, str):
                continue
            # A relative path is resolved against the directory of the declaration that contains
            # it, as the specification requires and the lint checks.
            wanted.append((key, lint.resolve(declaration.parent, value)))
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
    if candidate.is_file() or candidate.is_dir():
        return candidate
    if len(names.get(inside.name, ())) > 1:
        paths = ", ".join(sorted(str(path) for path in names[inside.name]))
        raise SystemExit(f"{paths} share the name {inside.name}; arrange {models} in the "
                         f"package layout so that each file is found at its own path")
    candidate = models / inside.name
    return candidate if candidate.is_file() or candidate.is_dir() else None


def assemble(declarations: Path, variant: str, models: Path, output: Path,
             license_file: Path | None) -> tuple[Path, str, str, dict, list[Path]]:
    """Copies the declaration and the files it needs into a package directory.

    :returns: the package directory, the package id, the package version, the package manifest as
              read, and every packaged model file, as paths inside the package.
    """
    # Resolved, because the declaration reader resolves the paths a declaration names: a relative
    # --declarations would otherwise compare resolved paths against an unresolved source.
    source = (declarations / variant).resolve()
    if not source.is_dir():
        raise SystemExit(f"no declaration for variant '{variant}' under {declarations}")
    lint = load_lint()
    desc = lint.load_json(source / "desc.json")
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
        if candidate.is_dir():
            # A directory a declaration names is one unit the engine reads whole, the dictionaries
            # of the tifa-ggml variant above all, so it travels with every file it holds. The
            # destination may already exist because an earlier key shipped a file inside it, so the
            # copy merges rather than refuses.
            shutil.copytree(candidate, directory / inside, dirs_exist_ok=True)
        else:
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
        for companion in companion_files(lint.load_json(directory / inside), inside.parent):
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
    """Runs the declaration lint on an assembled package and forwards its output.

    The lint is asked not to require a project above the package, because --output is often a
    directory outside the tree that states the version. The comparison still runs whenever a project
    is found, so the option only turns a missing project from an error into a warning.
    """
    result = subprocess.run(
        [sys.executable, str(SCRIPTS / "check-declarations.py"), "--no-project", str(directory)],
        capture_output=True,
        text=True,
    )
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        raise SystemExit(f"the declaration lint rejects {directory}")


def archive(directory: Path, version: str, output: Path) -> Path:
    """Writes the package directory as a zip archive whose entries share one root directory.

    The entries are ordered by their path inside the archive as a POSIX string. Sorting the paths
    themselves would order them by the rules of the running platform, which fold case on Windows
    and compare bytes elsewhere, so one set of files would travel as two archives whose entries
    differ in order, and two hosts reading the same release would see two listings.
    """
    root = directory.name
    target = output / f"{root}-{version}.zip"
    members = sorted((path for path in directory.rglob("*") if path.is_file()),
                     key=lambda path: path.relative_to(directory).as_posix())
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as bundle:
        for member in members:
            relative = member.relative_to(directory).as_posix()
            info = zipfile.ZipInfo(f"{root}/{relative}", date_time=FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            # The creating system is part of the entry header and the library derives it from the
            # platform it runs on, so the same input would travel as a different archive depending
            # on where it was assembled. The value is pinned to Unix, which is the system the
            # permission bits above belong to.
            info.create_system = 3
            # The size is set before the entry is opened, as writestr does, so the entry header is
            # identical and the file is streamed in chunks instead of being read in full.
            info.file_size = member.stat().st_size
            with member.open("rb") as source, bundle.open(info, "w") as entry:
                shutil.copyfileobj(source, entry, CHUNK)
    return target


def project_version(start: Path = REPOSITORY) -> str:
    """Returns the version the project declares, which is the version of every bundle it releases.

    The library, its packages and the manifest of a release carry one version, stated in the
    project() call of the top level CMakeLists.txt. Reading it here is what keeps the three from
    drifting: the packages are compared with the same value by the declaration lint.
    """
    found = projectversion.find(start)
    if found is None:
        raise SystemExit(f"no CMakeLists.txt declares a project version at or above {start}")
    return found[1]


def bundle_version(given: str, version: str) -> str:
    """Returns the version that --bundle states, or exits when it states another one.

    The release tag is the version without its trailing zero components, and it is the name the
    release is published under, so both writings name the same release and both are accepted here.
    """
    tag = projectversion.release_tag(version)
    if given not in (version, tag):
        raise SystemExit(f"--bundle {given} is neither the version of this project ({version}) nor "
                         f"the release tag of it ({tag}, which is that version without its trailing "
                         f"zero components)")
    return version


def merge_manifest(path: Path, fragment: dict, identifier: str, bundle: str) -> None:
    """Merges the manifest fragment of one package into the manifest of a release.

    The manifest is created when it does not exist yet, and the entry of the package is replaced
    rather than added, so that assembling the same package twice leaves one entry behind.

    A manifest describes one release: it announces one bundle version and every package it lists
    carries that same version. Writing the version of this package over a manifest of another
    release, or leaving entries of another version beside it, would publish a manifest that claims
    two releases at once, and a host reading it would install a package whose version the bundle
    does not announce.

    A merge replaces the entry of a package inside the list the manifest holds, so a manifest that
    states no packages at all, states null, or states anything that is not a list is refused while
    it is read: the merge names the file and the list it lacks rather than raising over it, and it
    writes nothing.
    """
    if path.exists():
        manifest = json.loads(path.read_text(encoding="utf-8"))
        announced = manifest.get("bundleVersion")
        if not announced:
            raise SystemExit(f"{path} contains no bundleVersion")
        if announced != bundle:
            raise SystemExit(f"{path} announces bundleVersion {announced} while this package is "
                             f"built from {bundle}")
        packages = manifest.get("packages")
        if not isinstance(packages, list):
            raise SystemExit(f"{path} lists no packages to merge an entry into (its packages are "
                             f"{type(packages).__name__}, which is not a list), so the entry of "
                             f"{identifier} cannot be written there")
        foreign = [entry for entry in packages
                   if entry.get("id") != identifier and entry.get("version") != bundle]
        if foreign:
            listed = ", ".join(f"{entry.get('id')} {entry.get('version')}" for entry in foreign)
            raise SystemExit(f"{path} carries packages of another version ({listed}) while this "
                             f"package is built from {bundle}")
    else:
        packages = []
        manifest = {"bundleVersion": bundle, "packages": packages}
    manifest["packages"] = sorted([entry for entry in packages if entry.get("id") != identifier]
                                  + [fragment], key=lambda entry: entry["id"])
    path.write_text(json.dumps(manifest, indent=4) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--declarations", default=DEFAULT_DECLARATIONS, type=Path,
                        help="directory holding one subdirectory per variant, each with its "
                             "desc.json; the declarations are not tracked in the repository "
                             "(default: the packages/ directory below the repository root)")
    parser.add_argument("--variant", required=True, help="subdirectory of --declarations to assemble")
    parser.add_argument("--models", required=True, type=Path,
                        help="directory containing the model files, searched by package path and then by file name")
    parser.add_argument("--output", default=REPOSITORY / "build" / "packages", type=Path,
                        help="output directory (default: build/packages)")
    parser.add_argument("--license", dest="license_file", type=Path,
                        help="file packaged as LICENSE if the model's own archive contains none")
    parser.add_argument("--manifest", type=Path,
                        help="merge the fragment into this manifest, creating it if absent")
    parser.add_argument("--bundle",
                        help="the version of the release, either as the version the project "
                             "declares or as the release tag of it (models-v0.1 for 0.1.0.0); "
                             "stating it here only says what it has to be")
    args = parser.parse_args()

    # A --bundle is checked as soon as it is given, because the whole point of the argument is to
    # confirm the version before anything is written. The other paths read the version when they
    # need it, so --help and a rejected --bundle work in a tree that holds no project.
    bundle = None
    if args.bundle is not None:
        bundle = bundle_version(args.bundle, project_version())

    args.output.mkdir(parents=True, exist_ok=True)
    directory, identifier, version, desc, shipped = assemble(
        args.declarations, args.variant, args.models, args.output, args.license_file)
    lint(directory)
    target = archive(directory, version, args.output)

    # The release is named after the version of the project, which is also the version the manifest
    # of the release carries, so it is read here rather than before the work of assembling.
    if bundle is None:
        bundle = project_version()
    tag = projectversion.release_tag(bundle)

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
        merge_manifest(args.manifest, fragment, identifier, bundle)

    print(json.dumps(fragment, indent=4))
    print(f"wrote {target}")
    print(f"release tag {tag}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
