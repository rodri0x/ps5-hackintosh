#!/usr/bin/env python3
"""Reads the console's /proc/iomem on stdin and prints the physical base of the VRAM carve-out (0x...), e.g.
    ssh root@console cat /proc/iomem | python3 tools/carveout.py
The PS5 Linux loader grows the carve-out downward from the fixed top 0x470000000 (vram.txt: 0x20000000 = 512 MB -> 0x450000000,
0x100000000 = 4 GiB -> 0x370000000); /proc/iomem lists it as a top-level 'Reserved' range ending at 0x46fffffff."""
import re, sys

base = None
zeros = False
for line in sys.stdin:
    # top-level lines only (nested ones are indented); older kernels print "reserved" in lower case
    m = re.match(r"^([0-9a-f]+)-([0-9a-f]+) : reserved", line, re.I)
    zeros = zeros or line.startswith("00000000-00000000 ")
    if m and int(m.group(2), 16) == 0x46FFFFFFF:
        base = int(m.group(1), 16)
if base is None:
    if zeros:
        sys.exit("/proc/iomem shows only zero addresses: it was read without root, read it as root on the console")
    sys.exit("no Reserved range ending at 0x46fffffff in /proc/iomem")
print(hex(base))
