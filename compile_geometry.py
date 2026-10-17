# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 TheSameEli
"""Compile geometry hints from the station's original DMI frames (no new assets)."""
from pathlib import Path
import re
import argparse
import os
from asset_utils import fingerprint, alpha_fingerprint
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--assets-root', type=Path, default=Path(os.environ.get('BANDASTATION_ROOT', Path(__file__).resolve().parent.parent)), help='BandaStation checkout containing icons/ and modular_bandastation/')
args = parser.parse_args()
ROOT = args.assets_root.resolve()
OUTPUT = Path(__file__).resolve().parent
for required in ('icons', 'modular_bandastation'):
    if not (ROOT / required).is_dir():
        parser.error(f'Missing {required}/ in {ROOT}; pass --assets-root or set BANDASTATION_ROOT')


entries = {}
ceiling_frames = set()
outdoor_frames = set()
lava_frames = set()
rock_frames = set()
natural_frames = set()
bench_frames = {}
shape_entries = {}
computer_fronts = {}
computer_pixels = {}


paths = list((ROOT / 'icons').rglob('*.dmi')) + list((ROOT / 'modular_bandastation').rglob('*.dmi'))
for path in sorted(paths):
    name = path.relative_to(ROOT).as_posix().lower()
    rock_source = '/turf/' in name and ('/walls/' in name and ('rock' in path.stem or path.stem in ('shale', 'volcanic')) or path.stem in ('mining', 'smoothrocks'))
    terrain_source = '/turf/' in name and '/walls/' not in name
    ceiling_source = (terrain_source or
                      '/aesthetics/floors/' in name or
                      name.startswith('icons/turf/floors/') and not any(token in path.stem for token in ('water', 'snow', 'lava', 'sand', 'grass', 'asteroid', 'hierophant')))
    if rock_source:
        kind = 48
    elif ceiling_source:
        kind = 26  # Only a ceiling eligibility hint; never object geometry.
    elif '/hud/' in name:
        kind = 19
    elif name == 'icons/obj/structures.dmi':
        kind = 7
    elif name == 'modular_bandastation/aesthetics/windows/icons/directional.dmi':
        kind = 32
    elif '/machines/atmospherics/' in name and path.stem in ('binary_devices', 'trinary_devices', 'unary_devices'):
        kind = 17
    elif path.stem == 'scangate' and '/machines/' in name:
        kind = 31
    elif name in ('icons/obj/doors/windoor.dmi', 'modular_bandastation/aesthetics/windoors/icons/windoors.dmi'):
        kind = 39
    elif path.stem == 'disposal' and ('/pipes_n_cables/' in name or '/aesthetics/disposal/' in name):
        kind = 23
    elif name in ('icons/obj/medical/cryogenics.dmi', 'icons/obj/machines/cloning.dmi'):
        kind = 40
    elif name == 'icons/obj/smooth_structures/catwalk.dmi':
        kind = 17
    elif '/smooth_structures/platform/' in name:
        kind = 44
    elif name == 'icons/obj/watercloset.dmi':
        kind = 37
    elif name in ('icons/ui_icons/minimap/map_blips.dmi', 'icons/ui_icons/minimap/map_blips_large.dmi'):
        kind = 33
    elif path.stem in ('stairs', 'small_stairs') and '/obj/' in name:
        kind = 38
    elif path.stem in ('trashcan', 'trashbin', 'trashcart', 'garbage') and '/obj/' in name:
        kind = 23
    elif name in ('icons/mob/effects/talk.dmi', 'icons/mob/human/emote_visuals.dmi'):
        kind = 30
    elif name == 'icons/obj/bed.dmi':
        kind = 27
    elif name == 'icons/obj/medical/medical_bed.dmi':
        kind = 28
    elif name == 'icons/obj/machines/sleeper.dmi':
        kind = 29
    elif name == 'icons/obj/service/library.dmi':
        kind = 46
    elif path.stem == 'chairs_wide':
        kind = 8
    elif path.stem == 'chairs':
        kind = 9
    elif path.stem in ('railings', 'handrail'):
        kind = 10
    elif '/doors/' in name or any(part in name for part in ('/airlocks/', '/windoors/', '/blastdoor/')):
        kind = 1
    elif name == 'icons/obj/machines/camera.dmi' or '/aesthetics/cameras/' in name:
        kind = 12
    elif (path.stem in ('wallmounts', 'airalarm', 'firealarm', 'lightswitch', 'signs', 'barsigns', 'status_display', 'intercom', 'apc', 'poster', 'posters', 'poster_wanted') or
          path.stem == 'flags' and ('/structures/' in name or '/fluff/' in name)):
        kind = 14
    elif path.stem == 'musician':
        kind = 13
    elif path.stem == 'microwave':
        kind = 15
    elif path.stem in ('computer', 'modular_console', 'nuke_terminal', 'pod_computer') and '/obj/' in name:
        kind = 25
    elif '/obj/storage/' in name and path.stem in ('closet', 'crates'):
        kind = 21 if path.stem == 'closet' else 22
    elif '/pipes_n_cables/' in name and path.stem in ('simple','manifold','!pipes_bitmask','!pipe_gas_overlays','pipe_underlays','layer_manifold_underlays','layer_cable','he-simple','he-manifold','he-junction','bridge_pipe','color_adapter','fluid_ducts','connects'):
        kind = 17
    elif path.stem in ('lighting', 'lighting_overlay', 'light') and ('/obj/' in name or '/lights/' in name):
        kind = 4
    elif ('window' in path.stem or 'grille' in path.stem) and ('structures' in name or '/windows/' in name or '/obj/' in name):
        kind = 7 if 'grille' in path.stem else (6 if 'window' in path.stem and ('/smooth_structures/' in name or '/bandastation/windows/' in name) else 2)
    elif '/smooth_structures/' in name and ('table' in path.stem or path.stem == 'rack'):
        kind = 11 if 'glass' in path.stem else 3
    elif path.stem in ('holopad', 'stairs', 'small_stairs', 'unary_devices', 'hologram'):
        kind = 4
    elif '/machines/' in name or '/machinery/' in name or '/vending/' in name:
        kind = 18
    else:
        continue
    im = Image.open(path).convert('RGBA')
    meta = Image.open(path).info.get('Description', '')
    w = re.search(r'^\s*width\s*=\s*(\d+)', meta, re.M)
    h = re.search(r'^\s*height\s*=\s*(\d+)', meta, re.M)
    w, h = int(w[1]) if w else 32, int(h[1]) if h else 32
    index = 0
    for block in re.split(r'^state\s*=', meta, flags=re.M)[1:]:
        state_name=block.splitlines()[0].strip().strip('"')
        eligible=kind == 17 or path.stem not in ('unary_devices','hologram') or any(token in state_name.lower() for token in ('vent','scrubber','holopad'))
        if kind == 48:
            eligible = '/walls/' in name or state_name.startswith(('rock', 'icerock', 'mountainrock', 'red_rock')) and state_name not in ('rockyash',)
        if kind == 26:
            eligible = bool(state_name) and not any(token in state_name.lower() for token in (
                'grass', 'basalt', 'sand', 'moon', 'wasteland', 'water', 'dirt', 'plasma',
                'lava', 'ash', 'shale', 'siderite', 'volcanic', 'hay', 'asphalt', 'snow', 'hull', 'solarpanel', 'lattice',
                'noop', 'invisible', 'overlay', 'thermite', 'shaft'))
            eligible = eligible and not any(token in path.stem for token in ('space', 'asteroid', 'lava', 'snow', 'sand', 'grass', 'water', 'planet'))
        if kind == 23 and path.stem == 'disposal':
            eligible = state_name in ('trashcan', 'disposal', 'disposal-flush', 'condisposal') or state_name.startswith(('dispover-', 'pipe', 'conpipe'))
        if kind == 46:
            eligible = state_name == 'bookempty' or state_name.startswith('book-') or state_name == 'book_connect'
        if kind == 37:
            eligible = state_name.startswith(('bathroom-', 'bounty-', 'cur_fancy-', 'shower', 'water', 'mist', 'mirror', 'magic_mirror', 'sink')) and not state_name.endswith(('frame', 'mask'))
        if kind == 33:
            eligible = state_name in ('stairs_up', 'stairs_down')
        if kind == 27:
            eligible = state_name in ('bed', 'abed', 'bed_double', 'dogbed', 'dirty_mattress')
        if kind == 28:
            eligible = state_name.startswith(('med_', 'emerg_', 'brakes_')) and 'folded' not in state_name
        if kind == 29:
            eligible = not state_name.startswith('console')
        if kind == 19:
            eligible = state_name in ('hand_l','hand_r','lhandactive','rhandactive')
        if kind == 7:
            eligible = 'grille' in state_name.lower()
        if kind == 8:
            eligible = any(token in state_name.lower() for token in ('sofa', 'bench', 'pew'))
        if kind == 9:
            eligible = not any(token in state_name.lower() for token in ('toppled', 'folded', 'shock', 'slide'))
        if kind == 12:
            eligible = state_name.startswith('camera') and 'case' not in state_name
        if kind == 13:
            eligible = state_name.startswith(('piano', 'grand'))
        if kind == 4 and path.stem in ('lighting', 'lighting_overlay', 'light'):
            eligible = state_name.startswith(('floor','tube','bulb'))
            frame_kind = 35 if state_name.startswith('floor') else 36
        else:
            frame_kind = kind
        if path.stem == 'sec' and state_name.startswith('recharger'):
            frame_kind = 53
        if kind == 1 and ('/multi_tile/' in name or '/airlocks/icons/large/' in name):
            frame_kind = 52
        if kind == 9 and not state_name.startswith(('comfychair', 'carp_chair', 'shuttle_chair', 'echair')):
            frame_kind = 51
        if kind == 23 and path.stem == 'disposal' and state_name.startswith(('pipe', 'conpipe')):
            frame_kind = 17
        if kind == 39 and state_name.endswith('open'):
            frame_kind = 49
        if path.stem == 'recycling' and state_name.startswith('conveyor'):
            frame_kind = 17
        if kind == 37:
            if state_name.startswith(('shower', 'water', 'mist')): frame_kind = 41
            elif state_name.startswith(('mirror', 'magic_mirror')): frame_kind = 42
            elif state_name.startswith('sink'): frame_kind = 43
        if path.stem == 'lathes' and state_name.startswith(('pipe_d', 'disposal_d', 'transit_d')):
            frame_kind = 4
        if kind == 25 and state_name.startswith(('arcade', 'invaders')):
            frame_kind = 45
        if kind == 22 and state_name.startswith('trashbin'):
            frame_kind = 23
        if kind == 1 and ('firedoor' in path.stem.lower() or 'doorfire' in path.stem.lower()):
            frame_kind = 50 if state_name.endswith('_open') else 34
        if kind == 8 and state_name.startswith('bench_'):
            frame_kind = 24
        if kind == 19:
            frame_kind = 20 if state_name in ('lhandactive','rhandactive') else 19
        dirs = re.search(r'^\s*dirs\s*=\s*(\d+)', block, re.M)
        frames = re.search(r'^\s*frames\s*=\s*(\d+)', block, re.M)
        dirs, frames = int(dirs[1]) if dirs else 1, int(frames[1]) if frames else 1
        for frame in range(frames):
            for direction in range(dirs):
                x, y = (index % (im.width // w)) * w, (index // (im.width // w)) * h
                tile = im.crop((x, y, x+w, y+h))
                index += 1
                if tile.getbbox():
                    key = fingerprint(tile)
                    if rock_source and eligible: rock_frames.add(key)
                    if terrain_source:
                        if 'lava' in state_name.lower() and 'mask' not in name and 'plasma' not in state_name.lower(): lava_frames.add(key)
                        if any(word in (name + '/' + state_name.lower()) for word in ('basalt', 'asteroid', 'ashplanet', 'rock', 'wasteland', 'sand', 'dirt', 'ash', 'shale', 'siderite', 'volcanic')): natural_frames.add(key)
                if (kind == 26 or kind == 48 and terrain_source) and tile.getbbox() and not eligible:
                    outdoor_frames.add(fingerprint(tile))
                if not tile.getbbox() or not eligible:
                    continue
                # Explicit E/W frames run along Y. Single-direction doors use wall supports.
                axis = 2 if dirs >= 4 and direction in (2, 3) else (1 if dirs >= 4 else 0)
                if kind in (8, 9, 10, 12, 25, 32, 39):
                    axis = direction + 1 if dirs >= 4 else 1
                if kind == 19:
                    axis = 1 if state_name in ('hand_l','lhandactive') else 2
                if kind == 33:
                    axis = 1 if state_name == 'stairs_up' else 2
                if path.stem == 'chairs_wide' and state_name.startswith('bench_'):
                    bench_frames.setdefault(state_name, {})[direction] = (tile, axis)
                key = fingerprint(tile)
                if (kind in (25, 14) or frame_kind in (36, 52)) and dirs == 4:
                    front_index = index - direction - 1
                    front_x, front_y = (front_index % (im.width//w))*w, (front_index//(im.width//w))*h
                    front = im.crop((front_x, front_y, front_x+w, front_y+h))
                    front_key = fingerprint(front)
                    computer_pixels.setdefault(front_key, front.tobytes())
                    computer_fronts.setdefault(key, front_key)
                if frame_kind in (37, 41, 42, 43):
                    shape_entries.setdefault(alpha_fingerprint(tile), (frame_kind, axis))
                if kind == 26:
                    ceiling_frames.add(key)
                    continue
                previous = entries.get(key)
                if frame_kind == 49 and (not previous or previous[0] != 49):
                    entries[key] = (frame_kind, axis) # Shared opening-end bitmap must retain the open-door geometry.
                elif previous and previous[0] == frame_kind and previous[1] != axis:
                    entries[key] = (frame_kind, 0) # Identical art cannot identify a direction.
                else:
                    entries.setdefault(key, (frame_kind, axis))
# Benches use SSgreyscale: the transmitted bitmap is a colored composite of
# base and cover, not either source DMI frame. Identify its combined alpha
# silhouette so arbitrary bench colors keep the same geometry and direction.
for state, directions in bench_frames.items():
    if not state.endswith('_base'):
        continue
    cover = bench_frames.get(state[:-5] + '_cover', {})
    for direction, (base, axis) in directions.items():
        if direction not in cover:
            continue
        combined = Image.alpha_composite(base, cover[direction][0])
        key = alpha_fingerprint(combined)
        previous = shape_entries.get(key)
        shape_entries[key] = (24, 0 if previous and previous[1] != axis else axis)
out = OUTPUT / 'geometry.generated.h'
out.write_text('// Generated by compile_geometry.py from original DMI assets.\n'
               'struct GeometryHint { unsigned long long hash; unsigned char kind,axis; };\n'
               'static constexpr GeometryHint geometryHints[] = {\n' +
               ''.join(f'{{0x{k:016x}ull,{v[0]},{v[1]}}},\n' for k,v in sorted(entries.items())) + '};\n'
               'static constexpr GeometryHint benchShapeHints[] = {\n' +
               ''.join(f'{{0x{k:016x}ull,{v[0]},{v[1]}}},\n' for k,v in sorted(shape_entries.items())) + '};\n' +
               'static constexpr unsigned long long ceilingFrames[] = {\n' +
               ''.join(f'0x{k:016x}ull,\n' for k in sorted(ceiling_frames)) + '};\n' +
               'static constexpr unsigned long long outdoorFrames[] = {\n' +
               ''.join(f'0x{k:016x}ull,\n' for k in sorted(outdoor_frames-ceiling_frames)) + '};\n')
front_offsets = {}
front_data = bytearray()
for key, data in sorted(computer_pixels.items()):
    front_offsets[key] = len(front_data)
    front_data.extend(data)
with out.open('a') as header:
    header.write('struct ComputerFrontHint { unsigned long long hash; unsigned offset; };\nstatic constexpr ComputerFrontHint computerFrontHints[] = {\n')
    header.write(''.join(f'{{0x{key:016x}ull,{front_offsets[value]}}},\n' for key,value in sorted(computer_fronts.items())))
    header.write('};\nstatic constexpr unsigned char computerFrontPixels[] = {\n')
    header.write(','.join(str(byte) for byte in front_data))
    header.write('};\n')
print(f'{len(entries)} geometry fingerprints -> {out}')
metal = Image.open(ROOT / 'icons/turf/walls/wall.png').convert('RGBA').crop((0, 0, 32, 32))
(OUTPUT / 'metal.generated.h').write_text(
    '// Original station wall panel, compiled for the ceiling.\n'
    'static constexpr unsigned metalPanelPixels[1024] = {\n' +
    ','.join(f'0xff{r:02x}{g:02x}{b:02x}u' for r,g,b,a in zip(*[metal.tobytes()[c::4] for c in range(4)])) + '\n};\n')

with out.open('a') as header:
    for label, values in [('lavaFrames', lava_frames), ('rockFrames', rock_frames), ('naturalFrames', natural_frames)]:
        header.write('static constexpr unsigned long long ' + label + '[] = {\n' + ''.join(f'0x{k:016x}ull,\n' for k in sorted(values)) + '};\n')
# Compose the original transparent parallax star layers onto black.
sky_source = Image.open(ROOT / 'icons/effects/parallax.dmi').convert('RGBA')
sky = Image.new('RGBA', (480,480), (0,0,0,255))
for x in range(3): sky = Image.alpha_composite(sky, sky_source.crop((x*480,0,(x+1)*480,480)))
with (OUTPUT / 'space.generated.h').open('w') as header:
    header.write('// Original parallax.dmi layers 1..3, composed on black.\nstatic constexpr unsigned spacePixels[230400] = {\n')
    header.write(','.join(f'0xff{r:02x}{g:02x}{b:02x}u' for r,g,b,a in zip(*[sky.tobytes()[c::4] for c in range(4)])))
    header.write('};\n')

# Full lava cell surface: border outlines must not disguise a hazardous turf.
lava_source = Image.open(ROOT / 'icons/turf/floors/lava.dmi')
meta = lava_source.info['Description']; lava_source = lava_source.convert('RGBA'); index = 0
for block in re.split(r'^state\s*=', meta, flags=re.M)[1:]:
    state = block.splitlines()[0].strip().strip('"')
    count = int(re.search(r'frames\s*=\s*(\d+)', block)[1])
    if state == 'lava-255':
        data = []
        for frame in range(count):
            at = index + frame; x = at % (lava_source.width//32)*32; y = at // (lava_source.width//32)*32
            tile = lava_source.crop((x,y,x+32,y+32))
            data.extend(f'0xff{r:02x}{g:02x}{b:02x}u' for r,g,b,a in zip(*[tile.tobytes()[c::4] for c in range(4)]))
        (OUTPUT/'lava.generated.h').write_text('static constexpr unsigned lavaSurfacePixels[] = {' + ','.join(data) + '};\n')
        break
    index += count
