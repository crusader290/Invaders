#!/usr/bin/env python3
"""Extract routine and RAM-variable names from the Computer Archeology Space Invaders
listing (Code.md / RAMUse.md / Hardware.md from github.com/topherCantrell/computerarcheology,
content/Arcade/SpaceInvaders) into a symbols file for recomp8080.py --symbols.

usage: import_listing.py path/to/SpaceInvaders OUT.sym"""
import os, re, sys

src, out = sys.argv[1], sys.argv[2]
code = open(os.path.join(src, "Code.md"), encoding="utf-8").read().splitlines()
ram = open(os.path.join(src, "RAMUse.md"), encoding="utf-8").read().splitlines()
addr_re = re.compile(r"^([0-9A-F]{4}):")
label_re = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):\s*$")
syms = []
pending = None
for line in code:
    m = label_re.match(line)
    if m:
        pending = m.group(1)
        continue
    m = addr_re.match(line)
    if m and pending:
        syms.append(("code", int(m.group(1), 16), pending))
        pending = None
seen = set()
for line in ram:
    m = re.match(r"^\|([0-9A-F]{4})(?:[:-][0-9A-F]{4})?\|([A-Za-z_][A-Za-z0-9_]*)\|", line)
    if m:
        a = int(m.group(1), 16)
        if a not in seen:
            seen.add(a)
            syms.append(("ram", a, m.group(2)))
for line in open(os.path.join(src, "Hardware.md"), encoding="utf-8"):
    m = re.match(r"^\|\s*00([0-9A-F]{2})p([rw])\s*\|\s*([A-Za-z_0-9]+)", line)
    if m:
        syms.append(("in" if m.group(2) == "r" else "out", int(m.group(1), 16), m.group(3)))
with open(out, "w") as f:
    f.write("# Symbol names for the Space Invaders ROM (invaders.h/g/f/e), used to annotate the\n"
            "# generated C. Names come from Christopher Cantrell's commented disassembly at\n"
            "# https://computerarcheology.com/Arcade/SpaceInvaders/Code.html\n"
            "# (source: github.com/topherCantrell/computerarcheology). Regenerate with\n"
            "# tools/import_listing.py.\n")
    for kind, a, name in sorted(syms, key=lambda s: (s[0], s[1])):
        f.write(("%s %02x %s\n" if kind in ("in", "out") else "%s %04x %s\n") % (kind, a, name))
print("%d code labels, %d RAM names, %d ports" % (sum(s[0] == "code" for s in syms), sum(s[0] == "ram" for s in syms), sum(s[0] in ("in", "out") for s in syms)))
