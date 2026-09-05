#!/usr/bin/env python3
# 从 mini-os 磁盘镜像提取文件（FS: 块0超级块/1 inode位图/2 数据位图/3 inode表/4+ 数据）
import struct, sys

BLOCK = 4096
def rd(blk, off, n):
    return img[blk * BLOCK + off : blk * BLOCK + off + n]

def inode(i):
    b = rd(3, i * 64, 64)
    size, typ, links = struct.unpack_from('<IHH', b, 0)
    blocks = list(struct.unpack_from('<12I', b, 8))
    indirect, pad, mode = struct.unpack_from('<IHH', b, 56)
    return dict(size=size, typ=typ, links=links, blocks=blocks, indirect=indirect, mode=mode)

def read_file(ino):
    f = inode(ino)
    out = b''
    for b in f['blocks']:
        if b: out += rd(b, 0, BLOCK)
    if f['indirect']:
        ind = rd(f['indirect'], 0, BLOCK)
        for i in range(len(ind) // 4):
            b = struct.unpack_from('<I', ind, i * 4)[0]
            if b: out += rd(b, 0, BLOCK)
    return out[:f['size']]

def list_root():
    root = inode(0)
    out = b''
    for b in root['blocks']:
        if b: out += rd(b, 0, BLOCK)
    ents = {}
    for off in range(0, len(out), 32):
        name, typ, pad, ino = struct.unpack_from('<24sHHI', out, off)
        name = name.split(b'\0')[0].decode()
        if name:
            ents[name] = ino
    return ents

img = open(sys.argv[1], 'rb').read()
outdir = sys.argv[-1]
names = sys.argv[2:-1]
ents = list_root()
print('root entries:', sorted(ents.keys()))
for name in names:
    if name not in ents:
        print(f'  {name}: NOT FOUND'); continue
    data = read_file(ents[name])
    open(outdir + '/' + name, 'wb').write(data)
    print(f'  {name}: inode={ents[name]} size={len(data)}')
