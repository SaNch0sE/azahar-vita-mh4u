#!/usr/bin/env python3
# Copyright 2026 Azahar Emulator Project
# Licensed under GPLv2 or any later version
# Refer to the license.txt file included.
"""The serialized state against the savestate format it is (SaveStateFormat, src/core/savestate.h).

A savestate loads into any build of its format, so a change to what the state holds needs a new
format. This hashes the code that writes and reads the state, and src/core/savestate_layout.txt
records the hash for the format: the check fails where either differs, so each change to that code
gets a decision (still loads the old states: record it; doesn't: raise the format, then record it).

What is hashed, comments and spacing left out, under src/core, src/video_core, src/audio_core,
src/common and src/network: every function named serialize, save, load, save_construct_data or
load_construct_data that takes an Archive&, and every line that names a serialization macro
(BOOST_CLASS_..., ...SERIALIZ...). Not seen: a serialized member's type, or a struct read as bytes
(make_binary_object) changing its layout; those need a new format by hand.

    savestate_check.py SRC_DIR            # check; exit 1 with what to do
    savestate_check.py --update SRC_DIR   # record the hash for the format savestate.h has
"""

import hashlib
import pathlib
import re
import sys

DIRS = ["core", "video_core", "audio_core", "common", "network"]
SUFFIXES = {".h", ".hpp", ".cpp", ".inc"}
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"', re.S)
FUNC = re.compile(
    r"\b(?:serialize|save|load|save_construct_data|load_construct_data)\s*\(\s*Archive\s*&"
)
MACRO = re.compile(r"BOOST_CLASS_|SERIALIZ")
FORMAT = re.compile(r"constexpr\s+u32\s+SaveStateFormat\s*=\s*(\d+)\s*;")


def without_comments(text):
    return TOKEN.sub(lambda m: m.group(0) if m.group(0).startswith('"') else " ", text)


def matching(text, at, open_char, close_char):
    """The index of the close_char that closes the open_char at `at`."""
    depth = 0
    for i in range(at, len(text)):
        if text[i] == open_char:
            depth += 1
        elif text[i] == close_char:
            depth -= 1
            if depth == 0:
                return i
    raise ValueError(f"unbalanced {open_char} at {at}")


def pieces(text):
    """A file's serialization functions (definitions, not declarations) and macro lines."""
    found = []
    for m in FUNC.finditer(text):
        params_end = matching(text, text.index("(", m.start()), "(", ")")
        brace = text.find("{", params_end)
        semicolon = text.find(";", params_end)
        if brace < 0 or (0 <= semicolon < brace):
            continue
        found.append(text[m.start() : matching(text, brace, "{", "}") + 1])
    for line in text.replace("\\\n", " ").split("\n"):
        if MACRO.search(line) and not line.lstrip().startswith("#include"):
            found.append(line)
    return [" ".join(p.split()) for p in found]


def layout_hash(src):
    digest = hashlib.sha256()
    count = 0
    for directory in DIRS:
        for path in sorted((src / directory).rglob("*")):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            text = without_comments(path.read_text(encoding="utf-8", errors="replace"))
            for piece in pieces(text):
                digest.update(f"{path.relative_to(src).as_posix()}\0{piece}\0".encode())
                count += 1
    return digest.hexdigest(), count


def main(argv):
    update = argv[:1] == ["--update"]
    if update:
        argv = argv[1:]
    src = pathlib.Path(argv[0])
    record = src / "core" / "savestate_layout.txt"
    found = FORMAT.search((src / "core" / "savestate.h").read_text(encoding="utf-8"))
    if not found:
        print("savestate_check: no SaveStateFormat in src/core/savestate.h", file=sys.stderr)
        return 1
    format_now = int(found.group(1))
    digest, count = layout_hash(src)
    if update:
        record.write_text(
            "# The serialized state's code as tools/savestate_check.py hashes it, for the savestate\n"
            "# format in src/core/savestate.h (SaveStateFormat). Written by savestate_check.py --update.\n"
            f"format {format_now}\nhash {digest}\n",
            encoding="utf-8",
        )
        print(f"savestate_check: format {format_now} recorded ({count} pieces hashed)")
        return 0
    fields = dict(
        line.split(" ", 1)
        for line in record.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    )
    if int(fields["format"]) != format_now:
        print(
            f"savestate_check: SaveStateFormat is {format_now}, src/core/savestate_layout.txt "
            f"records format {fields['format']}: run tools/savestate_check.py --update src",
            file=sys.stderr,
        )
        return 1
    if fields["hash"] != digest:
        print(
            f"savestate_check: the code that serializes the state changed since format "
            f"{format_now} was recorded. If states saved before still load into this build, "
            f"record it with tools/savestate_check.py --update src; if not, raise SaveStateFormat "
            f"in src/core/savestate.h first.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
