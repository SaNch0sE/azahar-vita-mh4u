#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""Names the addresses of a crash report (crash.txt, or a console log holding one).

symbolize_crash.py REPORT ELF_OR_DIR...

The report's `build` field is the link's build ID: the ELF with that ID is found among the files and
directories given (a build's citra_vita, or the folder where released ELFs are kept) and resolves the
report's call chain and return addresses. The last report in the file is the one read.
"""
import pathlib
import re
import subprocess
import sys

READELF = "arm-vita-eabi-readelf"
ADDR2LINE = "arm-vita-eabi-addr2line"


def build_id(path):
    """The ELF's build ID, or None for a file that is not one or has none."""
    result = subprocess.run([READELF, "-n", str(path)], capture_output=True, text=True)
    match = re.search(r"Build ID: ([0-9a-f]+)", result.stdout)
    return match.group(1) if match else None


def candidates(paths):
    for path in map(pathlib.Path, paths):
        if path.is_dir():
            yield from (p for p in sorted(path.iterdir()) if p.suffix in ("", ".elf") and p.is_file())
        else:
            yield path


def find_elf(wanted, paths):
    return next((p for p in candidates(paths) if build_id(p) == wanted), None)


def sections(lines):
    """The (label, addresses) of the last report's chain and scan, each over its lines."""
    found = {}
    label = None
    for line in lines:
        match = re.search(r"crash: (call chain|return addresses):?(.*)", line)
        if match:
            label = match.group(1)
            found[label] = []
            rest = match.group(2)
        else:
            match = re.search(r"crash:((?: [0-9a-f]{8})+)\s*$", line)
            if not match or label is None:
                label = None
                continue
            rest = match.group(1)
        found[label] += re.findall(r"[0-9a-f]{8}", rest)
    return found


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    lines = pathlib.Path(sys.argv[1]).read_text(errors="replace").splitlines()
    ids = [m.group(1) for line in lines for m in [re.search(r"\breport: .*, build ([0-9a-f]+|none),", line)] if m]
    if not ids:
        sys.exit("no report line with a build ID: a report from before build IDs, or not a report")
    if ids[-1] == "none":
        sys.exit("this build has no build ID")
    elf = find_elf(ids[-1], sys.argv[2:])
    if elf is None:
        sys.exit(f"no ELF with build ID {ids[-1]} among {' '.join(sys.argv[2:])}")
    print(f"build {ids[-1]}: {elf}")
    for label, addresses in sections(lines).items():
        print(f"\n{label}:")
        # A return address names the call after it: the address before it names the call.
        before = [f"{int(a, 16) - 1:x}" for a in addresses]
        result = subprocess.run([ADDR2LINE, "-Cfps", "-e", str(elf), *before],
                                capture_output=True, text=True)
        for address, line in zip(addresses, result.stdout.splitlines()):
            print(f"  {address}  {line}")


if __name__ == "__main__":
    main()
