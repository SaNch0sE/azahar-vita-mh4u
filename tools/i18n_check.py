#!/usr/bin/env python3
# Copyright 2026 Azahar Emulator Project
# Licensed under GPLv2 or any later version
# Refer to the license.txt file included.
"""The Vita menus' translations against the text they translate (src/citra_vita/i18n.h).

Finds every literal the sources pass to Tr, TrFormat or Translatable (adjacent literals joined, as
the compiler joins them) and every entry of each language's table (i18n_*.cpp: the {"English",
"translation"} pairs of its std::to_array<Translation>), and fails where a literal has no entry, an entry no literal uses, an English
text has two entries, or a translation's fmt fields ({}, {:#010x}, ...) are not its English's: the
same fields in the same order, or all numbered ({0}, {1:#x}) with each English field's spec at its
number.

    i18n_check.py FRONTEND_DIR [MORE_SOURCES...]   # check; exit 1 with the problems
    i18n_check.py --missing FRONTEND_DIR [...]     # the literals without an entry, one a line
"""

import pathlib
import re
import sys

LITERAL = r'"(?:[^"\\]|\\.)*"'
RUN = rf"((?:{LITERAL}\s*)+)"
USE = re.compile(rf"\b(?:Tr|TrFormat|Translatable)\(\s*{RUN}(?=[,)])")
ENTRY = re.compile(rf"\{{\s*{RUN},\s*{RUN}\}}")
ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}


def joined(run):
    """The text of adjacent literals, escapes decoded."""
    text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', run))
    return re.sub(r"\\(.)", lambda m: ESCAPES[m.group(1)], text)


def fields(text):
    """A format's replacement fields as (number or None, spec), {{ and }} skipped."""
    found = []
    i = 0
    while i < len(text):
        if text.startswith("{{", i) or text.startswith("}}", i):
            i += 2
            continue
        if text[i] == "{":
            end = text.index("}", i)
            name, _, spec = text[i + 1 : end].partition(":")
            found.append((int(name) if name else None, spec))
            i = end + 1
            continue
        i += 1
    return found


def fields_match(english, translation):
    want = fields(english)
    got = fields(translation)
    if all(number is None for number, _ in got):
        return [spec for _, spec in want] == [spec for _, spec in got]
    if any(number is None for number, _ in got):
        return False
    return {n for n, _ in got} == set(range(len(want))) and all(
        want[n][1] == spec for n, spec in got
    )


def table_entries(table):
    """A table file's entries: the pairs inside its std::to_array<Translation>({...})."""
    text = table.read_text(encoding="utf-8")
    start = text.index("to_array<Translation>({")
    # The initializer ends on a line of its own ("})" also ends a format such as "({} of {})").
    return ENTRY.finditer(text, start, text.index("\n})", start))


def main(argv):
    missing_only = argv[:1] == ["--missing"]
    if missing_only:
        argv = argv[1:]
    frontend = pathlib.Path(argv[0])
    tables = sorted(frontend.glob("i18n_*.cpp"))
    sources = [
        p
        for p in sorted(frontend.glob("*.cpp")) + sorted(frontend.glob("*.h"))
        if p not in tables
    ] + [pathlib.Path(p) for p in argv[1:]]

    used = {}
    for path in sources:
        for m in USE.finditer(path.read_text(encoding="utf-8")):
            used.setdefault(joined(m.group(1)), path.name)

    problems = []
    if missing_only:
        table_keys = set()
        for table in tables:
            for m in table_entries(table):
                table_keys.add(joined(m.group(1)))
        for text in sorted(used):
            if text not in table_keys:
                print(text.encode("unicode_escape").decode("ascii"))
        return 0

    for table in tables:
        entries = {}
        for m in table_entries(table):
            english, translation = joined(m.group(1)), joined(m.group(2))
            if english in entries:
                problems.append(f"{table.name}: {english!r} has two entries")
            entries[english] = translation
            if english not in used:
                problems.append(f"{table.name}: {english!r} is no literal's (left over)")
            elif not fields_match(english, translation):
                problems.append(
                    f"{table.name}: the fields of {translation!r} are not those of {english!r}"
                )
        for english, where in sorted(used.items()):
            if english not in entries:
                problems.append(f"{table.name}: no entry for {english!r} ({where})")

    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
