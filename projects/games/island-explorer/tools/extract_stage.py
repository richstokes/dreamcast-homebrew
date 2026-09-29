# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy>=2,<3", "pillow>=11,<13"]
# ///
"""Rebuild local Emerald Coast assets from a Sonic Adventure PAL/US GDI.

No game data is downloaded. Source GD-ROMs are only opened read-only.
Ninja offsets: X-Hax/sa_tools GameConfig/DC_SA1/STG01.ini (rev 2026-09).
"""
from pathlib import Path
import argparse
import hashlib
import json
import math
import struct
from collections import Counter
import numpy as np
from disc import Disc, prs
from ninja import Ninja, transform
from pvr import archive

ROOT = Path(__file__).resolve().parents[1]
# Decorative and traversable SET objects. Gameplay hazards are deliberately omitted.
MODELS = {
    32:0x11ffac, 33:0x121214, 34:0x122414, 35:0x122c68,
    36:0x124ea8, 37:0x1247b4, 39:0x12c6ac, 41:0x184430,
    42:0x17e154, 43:0x17c814, 44:0x17cb00, 45:0x17cf6c, 46:0x17d258,
    47:0x181d74, 48:0x182514, 49:0x180e48, 50:0x181158,
    51:0x17ea3c, 52:0x17fdfc, 54:0x186d2c, 62:0x188ec4, 63:0x189400,
    64:0x189c00, 70:0x18e15c, 71:0x19b498, 73:0x16e078, 74:0x168b48,
}
SOLID = {36,37,39,41,42,43,44,45,46,54,59,66,71,72,73,74}


def props(ninja, data, bank):
    names = ninja.object_names()
    meshes, counts = [], Counter()
    for i in range(struct.unpack_from('<I', data)[0]):
        ident, rx, ry, rz, x, y, z, sx, sy, sz = struct.unpack_from('<4H6f', data, 32+i*32)
        ident &= 0xfff
        scale = (1+sx,1+sy,1+sz) if ident == 50 else (1,1,1)
        m = transform((x,y,z),(rx,ry,rz),scale)
        surface = 0x80000001 if ident in SOLID else 0x80000000
        label = f'prop_{i:03d}_{names[ident]}'
        offsets = []
        if ident in MODELS:
            offsets = [(MODELS[ident],m)]
        elif ident == 58:
            offsets = [([0x166438,0x167780,0x16973c,0x16adbc,0x16c680][abs(int(sx))%5],m)]
        elif ident == 59:
            offsets = [(0x174f68 if int(sx)%2 == 0 else 0x1795b4,m)]
        elif ident == 66:
            offsets = [(0x16eae4 if int(sy)%2 == 0 else 0x172044,m)]
        elif ident == 72:
            yaw=ry*math.tau/65536
            offsets = [(0x1708ac,m@transform((0,110,0))),
                       (0x16eae4,transform((x+math.sin(yaw)*73,y,z+math.cos(yaw)*73))),
                       (0x172044,transform((x-math.sin(yaw)*57,y,z-math.cos(yaw)*57)))]
        for off, mat in offsets:
            meshes.extend(ninja.model(off,mat,bank,surface,label))
        if offsets:
            counts[names[ident]] += 1
    return meshes,dict(counts)


def pack_world(meshes, path):
    vertices, indices, records = [], [], []
    for m in meshes:
        v = np.array([v[:3] for v in m['vertices']])
        center = (v.min(0)+v.max(0))/2
        radius = float(np.max(np.linalg.norm(v-center,axis=1)))
        m['center'], m['radius'] = center.tolist(),radius
        records.append(struct.pack('<4f4Ii3I',*center,radius,len(vertices),len(v),len(indices),len(m['triangles'])*3,
                                   m['texture'],m['material_flags'],m['surface'],1 if m['name'].startswith('prop_') else 0))
        assert len(v)<65536
        vertices.extend(m['vertices'])
        indices.extend(i for t in m['triangles'] for i in t)
    raw = bytearray(struct.pack('<4sIII',b'IEW1',len(records),len(vertices),len(indices)))
    raw.extend(b''.join(records))
    raw.extend(b''.join(struct.pack('<5fI',*v) for v in vertices))
    raw.extend(struct.pack(f'<{len(indices)}H',*indices))
    path.write_bytes(raw)
    return dict(meshes=len(records),vertices=len(vertices),triangles=len(indices)//3,bytes=len(raw))


def pack_shoreline(meshes, path):
    segments=[]; seen=set()
    for m in meshes:
        if not m['name'].startswith('land_') or not (m['surface']&1):continue
        for tri in m['triangles']:
            v=[np.array(m['vertices'][i][:3]) for i in tri]
            points=[]
            for a,b in zip(v,v[1:]+v[:1]):
                if (a[1]<.2)!=(b[1]<.2):
                    points.append(a+(b-a)*((.2-a[1])/(b[1]-a[1])))
            if len(points)!=2:continue
            a,b=points;dist=np.linalg.norm(b-a)
            if dist<.1:continue
            key=tuple(sorted(tuple(np.round(p,2)) for p in points))
            if key in seen:continue
            seen.add(key)
            pieces=max(1,int(np.ceil(dist/20)))
            for i in range(pieces):
                start=a+(b-a)*i/pieces;end=a+(b-a)*(i+1)/pieces
                segments.append((start[0],start[2],end[0],end[2]))
    path.write_bytes(struct.pack('<I',len(segments))+b''.join(struct.pack('<4f',*s) for s in segments))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gdi',type=Path,default=next((Path.home()/'Dropbox/Games/ROMs/DREAMCAST/Sonic Adventure (EU) # SDC').glob('*.gdi'),None))
    args=parser.parse_args()
    if not args.gdi:
        parser.error('Supply --gdi /path/to/Sonic-Adventure.gdi')
    extracted=ROOT/'assets/extracted'; generated=ROOT/'assets/generated'
    image_dir=extracted/'textures'
    for p in (extracted,generated,image_dir):p.mkdir(parents=True,exist_ok=True)
    disc=Disc(args.gdi)
    files=['STG01.PRS','BEACH01.PRS','BEACH02.PRS','OBJ_BEACH.PRS','BG_BEACH.PRS','BEACH_SEA.PRS','SET0100S.BIN','SET0101S.BIN']
    manifest=dict(source_disc=args.gdi.name,files={},stages=[])
    buffers={}
    for name in files:
        b=disc.get(name)
        manifest['files'][name]=dict(sha256=hashlib.sha256(b).hexdigest(),bytes=len(b))
        (extracted/name).write_bytes(b)
        buffers[name]=prs(b) if name.endswith('.PRS') else b
        if name.endswith('.PRS'):(extracted/(name[:-4]+'.bin')).write_bytes(buffers[name])
    ninja=Ninja(buffers['STG01.PRS'])
    # These table lengths and texture indices are version-sensitive: fail early.
    if ninja.unpack('H',0x81554)[0]!=445 or ninja.unpack('H',0xdeb60)[0]!=152:
        raise ValueError('Unsupported STG01 revision; expected SA1 retail PAL/US layout')
    banks={};textures=[]
    for name in ['BEACH01','BEACH02','OBJ_BEACH','BG_BEACH','BEACH_SEA']:
        banks[name]=len(textures)
        textures.extend(archive(buffers[name+'.PRS'],name,image_dir))
    # IET2 stores upload-ready hardware mip chains, including layout-2 padding.
    raw=bytearray(struct.pack('<4sI',b'IET2',len(textures)))
    raw.extend(bytes(len(textures)*24))
    for i,t in enumerate(textures):
        raw.extend(bytes((-len(raw))%32));off=len(raw)
        raw.extend(t['raw'])
        struct.pack_into('<6I',raw,8+i*24,t['width'],t['height'],t['pixel'],t['layout'],off,len(t['raw']))
    (generated/'textures.bin').write_bytes(raw)
    for act,off in enumerate([0x81554,0xdeb60]):
        meshes=list(ninja.stage(off,banks[f'BEACH0{act+1}']))
        pm,counts=props(ninja,buffers[f'SET010{act}S.BIN'],banks['OBJ_BEACH']);meshes.extend(pm)
        if act==0:
            # The long orca pier is spawned by original code, not the SET file.
            for address,position in [(0x1288d4,(2803,-1,365)),(0x12b1b0,(3993,-1,365))]:
                meshes.extend(ninja.model(address,transform(position),banks['OBJ_BEACH'],0x80000001,'ocean_pier'))
            for j in range(51):
                meshes.extend(ninja.model(0x12be80 if j%2==0 else 0x12bffc,transform((2898+j*20,8.5,365)),banks['OBJ_BEACH'],0x80000001,'ocean_pier_link'))
        for m in meshes:
            assert -1<=m['texture']<len(textures)
            assert all(np.isfinite(v[:5]).all() for v in m['vertices'])
        stats=pack_world(meshes,generated/f'coast{act}.bin');stats['props']=counts
        pack_shoreline(meshes,generated/f'foam{act}.bin')
        (extracted/f'coast{act}.json').write_text(json.dumps(meshes,separators=(',',':')))
        manifest['stages'].append(stats)
        print(f'Emerald Coast {act+1}: {stats["meshes"]} batches, {stats["triangles"]} triangles, {stats["bytes"]:,} bytes; {sum(counts.values())} placed props')
    sky=list(ninja.model(0x11eb08,bank=banks['BG_BEACH'],label='sky'))
    pack_world(sky,generated/'sky.bin')
    manifest['textures']=[{k:v for k,v in t.items() if k!='raw'} for t in textures]
    manifest['banks']=banks
    (extracted/'manifest.json').write_text(json.dumps(manifest,indent=2))
    (generated/'asset_ids.h').write_text('\n'.join(f'#define TEX_{k} {v}' for k,v in banks.items())+'\n'+f'#define TEXTURE_COUNT {len(textures)}\n')
    disc.file.close()
    print('Assets rebuilt locally. Texture payload:',len(raw),'bytes')

if __name__=='__main__':main()
