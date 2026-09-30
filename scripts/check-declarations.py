#!/usr/bin/env python3
"""Lints analysis packages before publication.

The lint rejects every declaration that the loader or a shipped variant rejects at load, so a
package that passes the lint loads without error. It checks the exports against docs/schemas, the
configuration against the keys each variant reads, the values fixed by each variant's model (sample
rate, frame interval, one channel), the import options, and the consistency of the two blocks with
the model's own files. It also checks two properties outside the loader's rules: every file a
declaration refers to exists and is non-empty, and the package declares the version fields that
upgrades require.

Usage:

    python3 scripts/check-declarations.py [--declarations-only] <package directory>...

The exit status is non-zero if any error is reported. Warnings do not affect the exit status.

With --declarations-only, the lint checks the declarations alone and opens no file they refer to.
CI checks the declarations in packages/ in this mode, because the repository holds them without the
model files; the model files are added when a package is assembled.
"""

import argparse
import json
import re
import pathlib
import sys
from pathlib import Path


def load_json(path):
    """Reads a JSON file under the loader's rules (the spec 2.4 JSON profile).

    A UTF-8 BOM and `//` and `/* */` comments outside strings are accepted. A repeated key
    invalidates the document, because the value that takes effect would otherwise depend on the
    parser.
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
                raise ValueError(f"{path}: duplicate key {key!r}")
            result[key] = value
        return result

    return json.loads("".join(out), object_pairs_hook=no_duplicates)

CATEGORY = "inference"

F0 = "org.openvpi.otter.inference.F0"
NOTE = "org.openvpi.otter.inference.Note"
ALIGN = "org.openvpi.otter.inference.Align"

# Configuration keys of each variant that name a file, split into required and optional keys.
MODEL_KEYS = {
    (F0, "rmvpe"): {"required": ["model"], "optional": []},
    (NOTE, "game"): {
        "required": ["encoder", "segmenter", "estimator", "boundaryToDuration"],
        # The alignment model. A package without it remains usable and declares the absence
        # through exports, so the absence is not an error.
        "optional": ["durationToBoundary"],
    },
    (ALIGN, "hfa"): {
        "required": ["model", "config", "vocab"],
        "optional": [],
    },
}

# All keys a variant's configuration may contain. The provider rejects any other key at load, so
# the lint rejects it as well.
CONFIGURATION_KEYS = {
    (F0, "rmvpe"): {"model"},
    (NOTE, "game"): {
        "encoder", "segmenter", "estimator", "boundaryToDuration", "durationToBoundary",
        "timestep", "languages", "scheduleStart",
    },
    (ALIGN, "hfa"): {
        "model", "config", "vocab", "languages",
    },
}

# Export values fixed by each variant's model. The interpreter rejects a declaration that
# contradicts them, because the front end of the graph runs at a single sample rate and hop size.
# Every shipped variant also feeds its model a single channel.
VARIANT_FACTS = {
    (F0, "rmvpe"): {"sampleRate": 16000, "interval": 0.01},
    (NOTE, "game"): {"sampleRate": 44100},
    (ALIGN, "hfa"): {},
}

# The largest value the readers accept for an integer that they narrow to int.
INT_MAX = 2**31 - 1

# The keys of an Align languages entry, mirroring docs/schemas/align-1-exports.schema.json.
ALIGN_LANGUAGE_KEYS = {"language", "scheme", "lyrics", "phonemes"}

# Keys the aligner reads from the model's vocab.json, with the required type of each.
HFA_VOCAB_KEYS = {"vocab": dict, "silent_phonemes": list, "non_lexical_phonemes": list}

# Type of the model code to which each variant's configuration maps a language identifier. The
# game model identifies languages by integer; the hfa model identifies its dictionaries by a string
# code. In both variants configuration.languages maps host identifiers to model codes, so the lint
# checks only the type of each value.
LANGUAGE_NUMBERING = {
    (NOTE, "game"): "int",
    (ALIGN, "hfa"): "string",
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
        "optional": {"channelCount", "maxSegmentDuration", "languages", "defaultLanguage",
                     "supportsKnownNotes", "knobs"},
        "knobs": {"boundaryThreshold": "knob", "boundaryRadius": "knob", "noteThreshold": "knob",
                  "notePresenceCutoff": "knob", "steps": "intKnob"},
    },
    ALIGN: {
        "required": {"sampleRate"},
        "optional": {"channelCount", "maxSegmentDuration", "languages", "defaultLanguage",
                     "nonSpeechPhonemes", "defaultNonSpeechPhonemes", "silenceLabel", "knobs"},
        "knobs": {"nonSpeechThreshold": "knob", "nonSpeechMinDuration": "knob",
                  "gapFill": "knob"},
    },
}

KNOWN_INTERFACES = {
    F0,
    NOTE,
    ALIGN,
    # Reserved. A declaration of a reserved interface is reported as unimplemented rather than as
    # an unknown contract.
    "org.openvpi.otter.inference.Transcribe",
}

IMPLEMENTED_INTERFACES = {F0, NOTE, ALIGN}


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

# The grammars synthrt's ContribLocator applies to a package id and to a contribution id.
SEGMENT = re.compile(r"[A-Za-z0-9_-]+")
PACKAGE_ID = re.compile(r"[A-Za-z0-9_-]+(/[A-Za-z0-9_-]+)*")


def well_formed_version(version: str) -> bool:
    """Returns whether a version matches the loader's grammar: one to four decimal components
    without leading zeros, each no larger than INT_MAX.

    The published packages use four components. The loader also accepts fewer components, and so
    does the lint, because a shorter version is not an error at load.
    """
    return (isinstance(version, str) and VERSION.fullmatch(version) is not None
            and all(int(part) <= INT_MAX for part in version.split(".")))


def positive_int(value) -> bool:
    """Returns whether the readers accept a value as a positive integer."""
    return not isinstance(value, bool) and isinstance(value, int) and 1 <= value <= INT_MAX


def resolve(base: Path, value: str) -> Path:
    """Resolves a declaration path as the readers do, treating \\ as a separator in addition to /."""
    return (base / value.replace("\\", "/")).resolve()


def order(left: str, right: str) -> int:
    """Compares two versions.

    Returns a negative number if left is older than right, zero if the versions are equal, and a
    positive number if left is newer. Missing components count as zero.
    """
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
            report.error(where, f"the knob {name} requires exactly one key, a boolean default")
        return
    if set(value) != {"minimum", "maximum", "default"}:
        report.error(where, f"the knob {name} requires exactly the keys minimum, maximum and default")
        return
    numeric = int if kind == "intKnob" else (int, float)
    for key, item in value.items():
        if isinstance(item, bool) or not isinstance(item, numeric):
            report.error(where, f"the knob {name}.{key} must be a{'n integer' if kind == 'intKnob' else ' number'}")
            return
        if kind == "intKnob" and not -INT_MAX <= item <= INT_MAX:
            report.error(where, f"the knob {name}.{key} is out of range")
            return
    if not value["minimum"] <= value["default"] <= value["maximum"]:
        report.error(where, f"the knob {name} must have minimum <= default <= maximum")


# Grammars of a language identifier and a scheme name, identical to the grammars wolf applies.
LANGUAGE = re.compile(r"[a-z]{3}")
SCHEME = re.compile(r"[a-z0-9]+(-[a-z0-9]+)*")


def declared_languages(interface: str, exports: dict) -> list:
    """Returns the language codes listed in the exports, for either entry form of the contract."""
    languages = exports.get("languages")
    if not isinstance(languages, list):
        return []
    if interface == ALIGN:
        return [entry.get("language") for entry in languages if isinstance(entry, dict)]
    return [code for code in languages if isinstance(code, str)]


def check_align_languages(where: str, languages, report: Report) -> None:
    """Checks an Align languages array: the language, scheme, lyrics form and phonemes of each
    entry."""
    if not isinstance(languages, list):
        report.error(where, "languages must be an array of objects")
        return
    seen = set()
    for entry in languages:
        if not isinstance(entry, dict):
            report.error(where, "each entry of languages must be an object")
            continue
        for key in entry:
            if key not in ALIGN_LANGUAGE_KEYS:
                report.error(where, f"{key} is not a valid key of a languages entry")
        language, scheme = entry.get("language"), entry.get("scheme")
        if not (isinstance(language, str) and LANGUAGE.fullmatch(language)):
            report.error(where, f"{language!r} is not an ISO 639-3 code of three lowercase letters")
        if not (isinstance(scheme, str) and SCHEME.fullmatch(scheme)):
            report.error(where, f"the scheme {scheme!r} must match [a-z0-9]+(-[a-z0-9]+)*")
        if entry.get("lyrics") not in ("scheme", "text"):
            report.error(where, f"lyrics of {language} must be scheme or text")
        phonemes = entry.get("phonemes")
        if not isinstance(phonemes, list) or not phonemes \
                or not all(isinstance(x, str) and x for x in phonemes):
            report.error(where, f"the phonemes of {language} must be a non-empty list of labels")
        elif len(set(phonemes)) != len(phonemes):
            report.error(where, f"the phonemes of {language} must not repeat")
        if (language, scheme) in seen:
            report.error(where, f"languages contains {language} {scheme} twice")
        seen.add((language, scheme))


def check_exports(where: str, interface: str, exports, report: Report) -> dict:
    """Checks the exports block against the contract and returns it, or an empty dict."""
    if not isinstance(exports, dict):
        report.error(where, "requires an exports object, which declares the audio format and the knobs")
        return {}
    keys = EXPORTS_KEYS[interface]
    for key in keys["required"]:
        if key not in exports:
            report.error(where, f"the exports require {key}")
    for key in exports:
        if key not in keys["required"] | keys["optional"]:
            report.error(where, f"{key} is not an exports key of {interface}")
    for key in ("sampleRate", "channelCount"):
        value = exports.get(key)
        if value is not None and not positive_int(value):
            report.error(where, f"{key} must be a positive integer no larger than {INT_MAX}")
    for key in ("interval", "maxSegmentDuration"):
        value = exports.get(key)
        if value is not None and (isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0):
            report.error(where, f"{key} must be a number greater than zero")
    if "supportsKnownNotes" in exports and not isinstance(exports["supportsKnownNotes"], bool):
        report.error(where, "supportsKnownNotes must be a boolean")
    languages = exports.get("languages")
    if languages is not None:
        if interface == ALIGN:
            check_align_languages(where, languages, report)
        elif not isinstance(languages, list) or not all(isinstance(x, str) for x in languages):
            report.error(where, "languages must be a list of ISO 639-3 codes")
        else:
            for code in languages:
                if not LANGUAGE.fullmatch(code):
                    report.error(where, f"{code!r} is not an ISO 639-3 code of three lowercase letters")
            if len(set(languages)) != len(languages):
                report.error(where, "languages must not repeat")
    listed = declared_languages(interface, exports)
    default = exports.get("defaultLanguage")
    if default is not None and not (isinstance(default, str) and LANGUAGE.fullmatch(default)):
        report.error(where, "defaultLanguage must be an ISO 639-3 code of three lowercase letters")
    elif listed and default is None:
        # The loader also rejects this case: a host may omit the language, and the language that
        # applies in that case is the declared defaultLanguage.
        report.error(where, "the exports list languages but declare no defaultLanguage")
    elif default is not None and default not in listed:
        report.error(where, f"the default language {default} is not among the listed languages")
    non_speech = exports.get("nonSpeechPhonemes")
    if non_speech is not None:
        if not isinstance(non_speech, list) or not all(isinstance(x, str) and x for x in non_speech):
            report.error(where, "nonSpeechPhonemes must be a list of non-empty identifiers")
        elif len(set(non_speech)) != len(non_speech):
            report.error(where, "nonSpeechPhonemes must not repeat")
    default_non_speech = exports.get("defaultNonSpeechPhonemes")
    if default_non_speech is not None:
        if not isinstance(default_non_speech, list):
            report.error(where, "defaultNonSpeechPhonemes must be a list")
        else:
            for label in default_non_speech:
                if label not in (non_speech or []):
                    report.error(where, f"the default non-speech phoneme {label} is not among "
                                        f"the declared nonSpeechPhonemes")
    if "silenceLabel" in exports and not (isinstance(exports["silenceLabel"], str)
                                          and exports["silenceLabel"]):
        report.error(where, "silenceLabel must be a non-empty string")
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


def check_variant_facts(where: str, interface: str, variant: str, exports: dict,
                        report: Report) -> None:
    """Checks the exports against the values fixed by the variant's model.

    The variant's interpreter rejects a declaration that contradicts these values.
    """
    for key, fixed in VARIANT_FACTS[(interface, variant)].items():
        value = exports.get(key)
        if value is not None and not isinstance(value, bool) and value != fixed:
            report.error(where, f"the {variant} variant fixes {key} at {fixed}; the exports "
                                f"declare {value}")
    channels = exports.get("channelCount", 1)
    if positive_int(channels) and channels != 1:
        report.error(where, f"the {variant} variant feeds its model one channel; the exports "
                            f"declare {channels}")
    if interface == ALIGN and "silenceLabel" not in exports:
        report.error(where, "the hfa variant requires silenceLabel, which separates words; the "
                            "exports declare none")


def check_imports(where: str, imports, local: dict, report: Report) -> None:
    """Checks the options of imports whose target is an analyzer of this package.

    Level 1 analysis contracts define no import options, and the analyzer's interpreter rejects an
    import that specifies any. Only a reference into this package can be resolved; a reference into
    a dependency names a module outside the lint's input.
    """
    if imports is None:
        return
    if not isinstance(imports, list):
        report.error(where, "imports must be an array")
        return
    for entry in imports:
        if not isinstance(entry, dict):
            continue
        reference = entry.get("ref")
        options = entry.get("options")
        if not isinstance(reference, str) or not reference.startswith(":"):
            continue
        category, _, identifier = reference[1:].partition("/")
        if category != CATEGORY or local.get(identifier) not in KNOWN_INTERFACES:
            continue
        if options is not None and options != {}:
            report.error(where, f"the import of {reference} specifies options, but analysis "
                                f"contracts define no import options")


def check_declaration(path: Path, report: Report, declarations_only: bool = False,
                      local: dict = None) -> None:
    where = str(path)
    try:
        declaration = load_json(path)
    except (OSError, ValueError) as problem:
        report.error(where, f"cannot be read: {problem}")
        return

    interface = declaration.get("interface")
    variant = declaration.get("variant")
    level = declaration.get("level")
    if not isinstance(interface, str) or not isinstance(variant, str):
        report.error(where, "requires string values for interface and variant")
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
        report.warn(where, f"no variant named {variant} implements {interface} in this repository")
        return

    exports = check_exports(where, interface, declaration.get("exports"), report)
    check_variant_facts(where, interface, variant, exports, report)
    check_imports(where, declaration.get("imports"), local or {}, report)

    configuration = declaration.get("configuration")
    if not isinstance(configuration, dict):
        report.error(where, "requires a configuration object")
        return
    for key in configuration:
        if key not in CONFIGURATION_KEYS[(interface, variant)]:
            report.error(where, f"{key} is not a key the {variant} configuration reads; "
                                f"the audio format and the knobs belong in exports")

    for key in keys["required"]:
        if key not in configuration:
            report.error(where, f"the configuration requires {key}")
    for key in keys["required"] + keys["optional"]:
        value = configuration.get(key)
        if value is None:
            continue
        if not isinstance(value, str):
            report.error(where, f"{key} must be a path string")
            continue
        if declarations_only:
            continue
        target = resolve(path.parent, value)
        if not target.is_file():
            report.error(where, f"{key} refers to a missing file: {value}")
        elif target.stat().st_size == 0:
            report.error(where, f"{key} refers to an empty file: {value}")

    numbering = configuration.get("languages")
    if interface == ALIGN and not (isinstance(numbering, dict) and numbering):
        # The aligner locates the dictionary of each language through this map, so a
        # configuration without the map cannot align any language.
        report.error(where, "the hfa configuration requires a non-empty languages object that "
                            "maps each language identifier in the exports to a model code")
    if (interface, variant) in LANGUAGE_NUMBERING:
        kind = LANGUAGE_NUMBERING[(interface, variant)]
        if numbering is not None and not isinstance(numbering, dict):
            report.error(where, "languages must be an object that maps language identifiers to model codes")
            numbering = {}
        elif isinstance(numbering, dict):
            for name, value in numbering.items():
                if kind == "int" and (isinstance(value, bool) or not isinstance(value, int)
                                      or value < 0):
                    report.error(where, f"the model code of {name} must be a non-negative integer")
                if kind == "string" and not (isinstance(value, str) and value):
                    report.error(where, f"the model code of {name} must be a non-empty string")
        # The two blocks must agree: every language listed in the exports requires a model code.
        # The provider rejects the same inconsistency at load; the lint reports it before
        # publication.
        declared = declared_languages(interface, exports)
        for language in declared:
            if isinstance(numbering, dict) and language not in numbering:
                report.error(where, f"the exports list {language}, but the configuration maps it "
                                    f"to no model code")
        # The game variant rejects this case at load: a model that numbers its languages has no
        # neutral fallback number, and without listed languages an execution has no number to
        # pass to the model.
        if interface == NOTE and numbering and not declared:
            report.error(where, "the configuration maps languages, but the exports list none")

    if interface == NOTE:
        timestep = configuration.get("timestep")
        if timestep is not None and (isinstance(timestep, bool)
                                     or not isinstance(timestep, (int, float)) or timestep <= 0):
            report.error(where, "timestep must be a number greater than zero")
        start = configuration.get("scheduleStart")
        if start is not None and (isinstance(start, bool) or not isinstance(start, (int, float))
                                  or not 0 <= start <= 1):
            report.error(where, "scheduleStart must be a number between 0 and 1")
        if exports.get("supportsKnownNotes") and "durationToBoundary" not in configuration:
            report.error(where, "the exports declare supportsKnownNotes, but the configuration names no durationToBoundary model")
        if "durationToBoundary" in configuration and not exports.get("supportsKnownNotes"):
            report.warn(where, "the package contains an alignment model, but the exports do not declare supportsKnownNotes, so hosts never use the model")
        if "durationToBoundary" not in configuration:
            report.warn(where, "the package contains no alignment model; transcription cannot be conditioned on known notes")

    if interface == ALIGN and not declarations_only:
        check_align_declaration(where, path, exports, configuration, numbering, report)


def check_align_declaration(where: str, path: Path, exports: dict, configuration: dict,
                            numbering, report: Report) -> None:
    """Checks an Align declaration against the model files it refers to.

    The aligner reads a vocabulary, a mel spectrogram configuration and one dictionary per
    language, all taken from the model's own export. The declaration does not name the
    dictionaries; the vocabulary names them, following the model's own convention. The path checks
    in check_declaration therefore do not cover every file a declaration depends on, and this
    function checks the remaining files.
    """
    vocab_path = configuration.get("vocab")
    config_path = configuration.get("config")
    if not isinstance(vocab_path, str) or not isinstance(config_path, str):
        return  # already reported as missing or malformed
    vocab_file = resolve(path.parent, vocab_path)
    config_file = resolve(path.parent, config_path)
    try:
        vocab = load_json(vocab_file)
        model_config = load_json(config_file)
    except (OSError, ValueError) as problem:
        report.error(where, f"cannot read the model's own files: {problem}")
        return
    if not isinstance(vocab, dict) or not isinstance(model_config, dict):
        report.error(where, "the model's vocab.json and config.json must each contain a JSON object")
        return

    mel = model_config.get("mel_spec_config")
    if not isinstance(mel, dict):
        report.error(where, "the model's config.json contains no mel_spec_config object")
        mel = {}
    for key in ("sample_rate", "hop_size"):
        if not positive_int(mel.get(key)):
            report.error(where, f"the model's mel_spec_config requires a positive integer {key}")
    if positive_int(mel.get("sample_rate")) and isinstance(exports.get("sampleRate"), int) \
            and mel["sample_rate"] != exports["sampleRate"]:
        report.error(where, f"the exports declare {exports['sampleRate']} Hz, but the model's "
                            f"config.json specifies {mel['sample_rate']} Hz")

    for key, kind in HFA_VOCAB_KEYS.items():
        if not isinstance(vocab.get(key), kind):
            report.error(where, f"the model's vocab.json requires {key} to be "
                                f"{'an array' if kind is list else 'an object'}")
    if isinstance(vocab.get("vocab"), dict):
        if not vocab["vocab"]:
            report.error(where, "the model's vocabulary is empty")
        for label, index in vocab["vocab"].items():
            if isinstance(index, bool) or not isinstance(index, int) or index < 0:
                report.error(where, f"the vocabulary entry {label} must map to a non-negative integer")
    for key in ("silent_phonemes", "non_lexical_phonemes"):
        if isinstance(vocab.get(key), list) and not all(isinstance(x, str) for x in vocab[key]):
            report.error(where, f"the model's {key} must contain only strings")

    dictionaries = vocab.get("dictionaries")
    dictionaries = dictionaries if isinstance(dictionaries, dict) else {}
    # The aligner loads a dictionary for each language the exports declare and for no other
    # language: a model code that the configuration maps for an undeclared language is never used.
    for language in declared_languages(ALIGN, exports):
        code = (numbering or {}).get(language)
        if not isinstance(code, str):
            continue
        name = dictionaries.get(code)
        if name is None:
            report.error(where, f"the model's vocabulary has no dictionary for the language "
                                f"{language} ({code})")
            continue
        dictionary = resolve(vocab_file.parent, name) if isinstance(name, str) else None
        if dictionary is None:
            report.error(where, f"the dictionary of {code} must name a file")
            continue
        if not dictionary.is_file():
            report.error(where, f"the dictionary {name} named by the vocabulary for {code} is "
                                f"missing from the directory of the vocabulary")

    labels = vocab.get("vocab")
    labels = labels if isinstance(labels, dict) else {}
    entries = exports.get("languages") if isinstance(exports.get("languages"), list) else []
    languages_seen = set()
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        language = entry.get("language")
        if language in languages_seen:
            report.error(where, f"the exports list {language} more than once, but the hfa variant "
                                f"has one dictionary per language")
        languages_seen.add(language)
        code = (numbering or {}).get(language)
        if not isinstance(code, str) or not isinstance(entry.get("phonemes"), list):
            continue
        prefix = code + "/"
        emitted = {label[len(prefix):] for label in labels if label.startswith(prefix)}
        promised = set(entry["phonemes"])
        if promised != emitted:
            report.error(where, f"the phonemes listed for {language} differ from the phonemes in "
                                f"the model vocabulary for {code}: "
                                f"not listed {sorted(emitted - promised)}, "
                                f"listed but unknown {sorted(promised - emitted)}")

    known_non_speech = vocab.get("non_lexical_phonemes")
    known_non_speech = known_non_speech if isinstance(known_non_speech, list) else []
    for label in exports.get("nonSpeechPhonemes") or []:
        if label not in known_non_speech:
            report.error(where, f"the exports declare the non-speech phoneme {label}, which is not "
                                f"among the model's non_lexical_phonemes")

    silent = vocab.get("silent_phonemes")
    silent = silent if isinstance(silent, list) else []
    label = exports.get("silenceLabel")
    if label is not None and label not in silent:
        report.error(where, f"the exports name {label} as the silence label, which is not "
                            f"among the model's silent_phonemes")
    if label is not None and label not in labels:
        report.error(where, f"the exports name {label} as the silence label, which is not a class "
                            f"of the model's vocabulary")


def check_package(root: Path, report: Report, declarations_only: bool = False) -> None:
    where = str(root)
    desc = root / "desc.json"
    if not desc.is_file():
        report.error(where, "has no desc.json")
        return
    try:
        manifest = load_json(desc)
    except (OSError, ValueError) as problem:
        report.error(str(desc), f"cannot be read: {problem}")
        return

    identifier = manifest.get("id")
    if not (isinstance(identifier, str) and PACKAGE_ID.fullmatch(identifier)):
        report.error(str(desc), f"the package id {identifier!r} must be segments of letters, "
                                f"digits, _ and - separated by /")

    version = manifest.get("version")
    if not well_formed_version(version):
        report.error(str(desc), "requires a version of one to four numbers without leading zeros")
    else:
        compat = manifest.get("compatVersion")
        if compat is None:
            # A package without compatVersion declares compatibility with no earlier version, so
            # a host that depends on the package cannot accept a newer build. The field costs
            # nothing before publication and is expensive to add after packages are distributed.
            report.warn(str(desc), "declares no compatVersion; hosts cannot accept upgrades of "
                                   "this package")
        elif not well_formed_version(compat):
            report.error(str(desc), "compatVersion must be a version of one to four numbers "
                                    "without leading zeros")
        elif order(compat, version) > 0:
            report.error(str(desc), f"compatVersion {compat} is newer than version {version}")

    entries = manifest.get("contributions", {}).get(CATEGORY)
    if not entries:
        report.error(str(desc), "declares no inference modules")
        return
    seen = set()
    declarations = []
    for entry in entries:
        identifier = entry.get("id")
        if not identifier:
            report.error(str(desc), "a contribution entry has no id")
            continue
        if not (isinstance(identifier, str) and SEGMENT.fullmatch(identifier)):
            report.error(str(desc), f"the contribution id {identifier!r} must be letters, "
                                    f"digits, _ and -")
            continue
        if identifier in seen:
            report.error(str(desc), f"two contributions share the id {identifier}")
        seen.add(identifier)
        relative = entry.get("path")
        if not relative:
            report.error(str(desc), f"{identifier} has no declaration path")
            continue
        declaration = resolve(root, relative)
        if not declaration.is_file():
            report.error(str(desc), f"{identifier} refers to a missing declaration")
            continue
        declarations.append((identifier, declaration))

    # Interface of every module in this package, used to check an import into the package against
    # the contract of its target.
    local = {}
    for identifier, declaration in declarations:
        try:
            interface = load_json(declaration).get("interface")
        except (OSError, ValueError, AttributeError):
            interface = None
        local[identifier] = interface
    for identifier, declaration in declarations:
        check_declaration(declaration, report, declarations_only, local)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("packages", nargs="+", type=Path,
                        help="package directory, the directory that contains desc.json")
    parser.add_argument("--declarations-only", action="store_true",
                        help="check the declarations without opening the files they refer to")
    args = parser.parse_args()

    report = Report()
    for root in args.packages:
        if not root.is_dir():
            report.error(str(root), "is not a directory")
            continue
        check_package(root, report, args.declarations_only)

    checked = len(args.packages)
    print(f"checked {checked} package(s): {report.errors} error(s), {report.warnings} warning(s)")
    return 1 if report.errors else 0


if __name__ == "__main__":
    sys.exit(main())
