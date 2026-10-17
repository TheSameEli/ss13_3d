# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 TheSameEli
"""Decode the proven subset of BYOND 516.1663/516.1688 MapIcon snapshots, not a full appearance ABI."""
from collections import Counter
import json
from pathlib import Path
import struct
import sys

source = Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).parent/'bin/scene.bin'
data = source.read_bytes()
magic, version, stride, roots = struct.unpack_from('<4I', data)
if magic != 0x53493242 or (version, stride) not in ((2,208),(2,216)):
    raise ValueError('Unsupported scene snapshot')
entries = struct.unpack_from('<I', data, 16)[0]
if len(data) != 20+entries*stride or roots > entries:
    raise ValueError('Incomplete snapshot; retry after capture finishes')
sprites = []
for i in range(entries):
    icon = data[20+i*stride:20+(i+1)*stride]
    x, y = struct.unpack_from('<hh', icon, 0x4c)
    plane = struct.unpack_from('<h', icon, 0x24)[0]
    first, end = struct.unpack_from('<HH', icon, (0xae if stride==216 else 0xa6))
    if not 0 <= first <= end <= entries:
        raise ValueError(f'Invalid child range at {i}')
    sprites.append(dict(index=i, root=i<roots, plane=plane, pixel_position=[x,y],
                        children=[first,end], raw_hex=icon.hex()))
report = dict(format='BYOND-516.1688-observed-subset' if stride==216 else 'BYOND-516.1663-observed-subset', roots=roots, entries=entries,
              root_planes=dict(Counter(s['plane'] for s in sprites[:roots])), sprites=sprites)
output = source.with_suffix('.json')
output.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(f'{roots} roots, {entries} total entries; decoded snapshot: {output}')
