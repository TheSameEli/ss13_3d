# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 TheSameEli
"""Read-only diagnostic of native PixBits in the client launched by this workspace."""
import ctypes
import re
import struct
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
log = (ROOT/'bin/scene-probe.log').read_text()
icons = int(re.search(r'driver icons=([0-9A-F]+)', log)[1],16)
pid = int(__import__('sys').argv[1])
k = ctypes.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.restype=ctypes.c_void_p
k.ReadProcessMemory.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p]
k.CloseHandle.argtypes=[ctypes.c_void_p]
process=k.OpenProcess(0x10,False,pid)
def read(address,n):
    data=ctypes.create_string_buffer(n)
    if not k.ReadProcessMemory(process,address,data,n,None): raise OSError(ctypes.get_last_error())
    return data.raw
def word(address):return struct.unpack('<I',read(address,4))[0]
header=read(int([v for v in re.findall(r'list=([0-9A-Fa-f]+)',log) if int(v,16)][-1],16),12)
roots,capacity=struct.unpack_from('<HH',header,8)
scene=read(struct.unpack_from('<I',header,4)[0],capacity*216)
array=word(icons+4)
assets={}
seen=set()
def walk(i):
    if i in seen:return
    seen.add(i)
    b=scene[i*216:(i+1)*216]
    image=struct.unpack_from('<I',b)[0]
    plane=struct.unpack_from('<h',b,36)[0]
    if image<65535 and (plane+int(__import__("math").floor((57-plane)/107))*107) in (-5,-4) and not b[40]&128 and image not in assets:
        pix=word(word(array+image*4))
        pointer,w,h,_=struct.unpack('<4I',read(pix,16))
        if 32<=w<=128 and 32<=h<=128:
            im=Image.frombytes('RGBA',(w,h),read(pointer,w*h*4))
            assets[image]=im
    start,end=struct.unpack_from('<HH',b,174)
    for j in range(start,end):walk(j)
for i in range(roots):walk(i)
k.CloseHandle(process)
assets=list(assets.items())[:120]
out=Image.new('RGB',(8*144,((len(assets)+7)//8)*160),'#333333')
draw=ImageDraw.Draw(out)
for i,(index,im) in enumerate(assets):
    x,y=(i%8)*144,(i//8)*160
    out.paste(im.resize((im.width*min(3,128//im.width),im.height*min(3,128//im.height)),Image.Resampling.NEAREST),(x,y+20),im.resize((im.width*min(3,128//im.width),im.height*min(3,128//im.height)),Image.Resampling.NEAREST))
    draw.text((x,y),str(index),fill='white')
out.save(ROOT/'bin/asset-atlas.png')
print(f'{len(assets)} original native assets')
