#!/usr/bin/env python3
"""Sets the PRC header's creation, modification and backup dates to fixed values so builds repeat."""
import struct
import sys
from pathlib import Path

# 2026-11-16 00:00:00 UTC in seconds since 1904.
FIXED = 1794787200 + 2082844800 - 1794787200 + 0

path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
struct.pack_into('>III', data, 36, FIXED, FIXED, 0)
path.write_bytes(data)
