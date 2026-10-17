# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 TheSameEli
"""Check original DMI frames against the generated renderer classifications."""
from pathlib import Path
from PIL import Image
import argparse
import os
import re
from asset_utils import fingerprint

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--assets-root', type=Path, default=Path(os.environ.get('BANDASTATION_ROOT', root.parent)))
args = parser.parse_args()
assets_root = args.assets_root.resolve()
header=(root/'geometry.generated.h').read_text().split('geometryHints[] = {')[1].split('};')[0]
hints={int(h,16):int(k) for h,k in re.findall(r'\{0x([a-f0-9]+)ull,(\d+),\d+\}',header)}
counts={49:0,17:0,23:0,50:0,51:0,14:0,52:0,53:0}
for path in ['modular_bandastation/aesthetics/airlocks/icons/large/large.dmi','modular_bandastation/aesthetics/airlocks/icons/large/overlays.dmi','icons/obj/doors/airlocks/multi_tile/public/glass.dmi','icons/obj/machines/sec.dmi','icons/obj/smooth_structures/catwalk.dmi','icons/obj/doors/doorfireglass.dmi','icons/obj/chairs.dmi','modular_bandastation/aesthetics/lightswitch/icons/lightswitch.dmi','modular_bandastation/aesthetics/airalarm/icons/airalarm.dmi','icons/obj/doors/windoor.dmi','modular_bandastation/aesthetics/windoors/icons/windoors.dmi','icons/obj/machines/recycling.dmi','icons/obj/pipes_n_cables/disposal.dmi','modular_bandastation/aesthetics/disposal/icons/disposal.dmi']:
    im=Image.open(assets_root/path);meta=im.info['Description'];im=im.convert('RGBA');w=int(re.search(r'width\s*=\s*(\d+)',meta)[1]);height=int(re.search(r'height\s*=\s*(\d+)',meta)[1]);index=0
    for block in re.split(r'^state\s*=',meta,flags=re.M)[1:]:
        name=block.splitlines()[0].strip().strip('"');d=re.search(r'dirs\s*=\s*(\d+)',block);f=re.search(r'frames\s*=\s*(\d+)',block);count=(int(d[1]) if d else 1)*(int(f[1]) if f else 1)
        expected=49 if 'windoor' in path and name.endswith('open') else 17 if name.startswith(('conveyor','pipe','conpipe')) else 23 if 'disposal' in path and (name in ('disposal','disposal-flush','trashcan','condisposal') or name.startswith('dispover-')) else 0
        if '/multi_tile/' in path or '/airlocks/icons/large/' in path: expected=52
        if '/catwalk.dmi' in path: expected=17
        if '/sec.dmi' in path and name.startswith('recharger'): expected=53
        if 'doorfire' in path and name.endswith('_open'): expected=50
        if '/chairs.dmi' in path and name in ('chair','wooden_chair','plastic_chair','stool'): expected=51
        if name and ('/lightswitch/' in path or '/airalarm/' in path): expected=14
        if expected:
            for at in range(index,index+count):
                x=at%(im.width//w)*w;y=at//(im.width//w)*height;tile=im.crop((x,y,x+w,y+height))
                if tile.getbbox():
                    assert hints.get(fingerprint(tile))==expected,(path,name,hints.get(fingerprint(tile)),expected)
                    counts[expected]+=1
        index+=count
assert all(counts.values())
print('Open windoors, flat belts/pipes and 3D disposal units:',counts)
