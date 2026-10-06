#!/usr/bin/env python3
"""Reads the metadata of a ggml model and prints what a package declaration needs.

A ggml package carries its model facts inside the GGUF file, which the declaration lint cannot
read: the exports block must be written from the model, not the other way around. This script
prints those facts so a declaration can be written against the model it names, in the same shape
the interpreters check when an analyzer is created.

Usage:

    python3 scripts/read-ggml-metadata.py --variant game <model.gguf>
    python3 scripts/read-ggml-metadata.py --variant tifa <model.gguf>

The game variant prints the audio format and the language numbering the model carries; the tifa
variant prints the audio format and, for every language the vocabulary prefixes, the phoneme list
the exports must declare. Both print the values only: assembling them into a declaration is the
caller's work, because which languages a package offers and which schemes it names are choices,
not facts.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

# Value types of the GGUF metadata, as the format specification numbers them.
U8, I8, U16, I16, U32, I32, F32, BOOL, STRING, ARRAY, U64, I64, F64 = range(13)

SCALAR = {
    U8: "<B",
    I8: "<b",
    U16: "<H",
    I16: "<h",
    U32: "<I",
    I32: "<i",
    F32: "<f",
    BOOL: "<?",
    U64: "<Q",
    I64: "<q",
    F64: "<d",
}


def read_exact(handle, length: int) -> bytes:
    """Returns exactly length bytes, or exits when the file ends inside a field.

    A truncated or corrupt file would otherwise hand a partial field to struct.unpack, which
    raises a bare struct.error, or to a string decode, which returns a short string with no sign
    that the file was cut short.
    """
    data = handle.read(length)
    if len(data) != length:
        raise SystemExit(f"the metadata ends inside a field after {length - len(data)} missing "
                         "bytes; the file is truncated or not a GGUF file")
    return data


def read_string(handle) -> str:
    (length,) = struct.unpack("<Q", read_exact(handle, 8))
    return read_exact(handle, length).decode("utf-8")


def read_value(handle, kind):
    if kind == STRING:
        return read_string(handle)
    if kind == ARRAY:
        element = struct.unpack("<I", read_exact(handle, 4))[0]
        (count,) = struct.unpack("<Q", read_exact(handle, 8))
        return [read_value(handle, element) for _ in range(count)]
    return struct.unpack(SCALAR[kind], read_exact(handle, struct.calcsize(SCALAR[kind])))[0]


def read_metadata(path: Path) -> dict:
    """Returns the metadata key-value pairs of a GGUF file, tensors untouched."""
    with path.open("rb") as handle:
        if read_exact(handle, 4) != b"GGUF":
            raise SystemExit(f"{path}: not a GGUF file")
        version, = struct.unpack("<I", read_exact(handle, 4))
        if version < 2:
            raise SystemExit(f"{path}: GGUF version {version} predates the metadata format this "
                             "script reads")
        tensor_count, kv_count = struct.unpack("<QQ", read_exact(handle, 16))
        if tensor_count:
            print(f"note: {path} carries {tensor_count} tensors, which this script skips",
                  file=sys.stderr)
        return {read_string(handle): read_value(handle, struct.unpack("<I", read_exact(handle, 4))[0])
                for _ in range(kv_count)}


def game_facts(metadata: dict) -> None:
    """Prints what a game-ggml exports block states about the audio format and the languages."""
    rate = metadata.get("game.inference.audio_sample_rate")
    lang_map = json.loads(metadata["game.inference.lang_map"]) if "game.inference.lang_map" in metadata else {}
    print("sampleRate:", rate)
    print("languages:", json.dumps(lang_map, ensure_ascii=False, sort_keys=True))
    print("hopSize:", metadata.get("game.inference.hop_size"),
          "(the frame rate the radius knob is stated against)")


def tifa_facts(metadata: dict) -> None:
    """Prints what a tifa-ggml exports block states about the audio format and the phonemes."""
    print("sampleRate:", metadata.get("tifa.features.audio_sample_rate"))
    print("hopSize:", metadata.get("tifa.features.hop_size"))

    vocabulary = json.loads(metadata["tifa.vocab.json"])
    symbols = vocabulary.get("symbols", {})
    # The symbols of one language are the ones its code prefixes; a symbol without a prefix is
    # shared, and any language's result may report it.
    prefixed: dict[str, list[str]] = {}
    shared: list[str] = []
    if isinstance(symbols, dict):
        items = symbols.items()
    else:
        items = ((symbol, number) for number, symbol in enumerate(symbols))
    for symbol, _ in items:
        if symbol == "<unused>":
            continue
        code, separator, rest = symbol.partition("/")
        if separator and rest:
            prefixed.setdefault(code, []).append(rest)
        else:
            shared.append(symbol)
    for code in sorted(prefixed):
        print(f"phonemes[{code}]:", json.dumps(sorted(prefixed[code]), ensure_ascii=False))
    if shared:
        print("shared:", json.dumps(sorted(shared), ensure_ascii=False))
    print("stopSymbols:", json.dumps(vocabulary.get("stop_symbols", []), ensure_ascii=False))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--variant", choices=["game", "tifa"], required=True)
    parser.add_argument("model", type=Path)
    arguments = parser.parse_args()

    metadata = read_metadata(arguments.model)
    print(f"# {arguments.model}")
    if arguments.variant == "game":
        game_facts(metadata)
    else:
        tifa_facts(metadata)
    return 0


if __name__ == "__main__":
    sys.exit(main())
