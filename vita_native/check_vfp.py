#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""check_vfp.py OBJDUMP ELF: fails when a function of the plugin outside ALLOWED holds a VFP/NEON
instruction. The kernel saves no VFP state for a plugin, and its interrupt handler, kernel threads
and exception vector run with someone else's registers live (CMakeLists.txt): only the functions
that work on VFP state on purpose may touch it."""
import re
import subprocess
import sys

# The guest's VFP bank saved and restored around a slice (entry.S, core2_install/uninstall), the
# FPSCR/FPEXC the vector reads, and the short-vector emulation that computes on the saved bank.
ALLOWED = {
    "azahar_enter", "azahar_exit_common", "azahar_svc_fast", "azahar_vfp_svc",
    "core2_install", "core2_uninstall",
}

objdump, elf = sys.argv[1], sys.argv[2]
out = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf], capture_output=True, text=True,
                     check=True).stdout
function = None
found = {}
for line in out.splitlines():
    m = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
    if m:
        function = m.group(1)
        continue
    m = re.match(r"^\s*[0-9a-f]+:\s+(v[a-z0-9.]+)\s", line)
    if m and function not in ALLOWED:
        found.setdefault(function, []).append(line.strip())
if found:
    for function, lines in found.items():
        print(f"check_vfp: {function} uses VFP/NEON ({len(lines)}): {lines[0]}", file=sys.stderr)
    sys.exit(1)
