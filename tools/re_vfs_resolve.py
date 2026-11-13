"""Applies the exact signature logic of plugins/content_packs/content_packs.c to victoria3.exe and
prints the resolved RVAs, so a game patch that breaks a signature shows up without launching the game.
Usage: python tools/re_vfs_resolve.py [path/to/victoria3.exe]     exit 0 = all resolved."""
import struct, sys
from re_vfs_lib import Image, DEFAULT_EXE

INIT_STR = '../platform_specific_game_data'                       # only referenced by the VFS init fn
MOUNTDATA_STR = 'No subdirs mounted for game dir from candidates: {}'  # MountData fn (physfs.cpp)
SCAN = 0x600


def rel32(b, at):
    return struct.unpack_from('<i', b, at)[0]


def resolve(im):
    def fstart(rva):
        return im.function_start(rva)

    def code(rva, n):
        o = rva - im.tva
        return im.tdata[o:o + n]

    out = {}
    s = im.find_string(INIT_STR)
    x = im.xrefs(s[0]) if s else []
    out['vfs_init'] = fstart(x[0]) if x else None
    s = im.find_string(MOUNTDATA_STR)
    x = im.xrefs(s[0]) if s else []
    md = fstart(x[0]) if x else None
    out['mount_data'] = md
    out['physfs_mount'] = out['refresh_search_path'] = None
    if md:
        b = code(md, SCAN)
        i = b.find(b'\x45\x33\xc0\xe8')                     # xor r8d,r8d ; call mount(_, dir, NULL)
        if i >= 0:
            out['physfs_mount'] = md + i + 8 + rel32(b, i + 4)
        i = b.find(b'\x45\x85\xff\x0f\x85')                  # test r15d,r15d ; jne <refresh call>
        if i >= 0:
            t = md + i + 9 + rel32(b, i + 5)
            tb = code(t, 5)
            if tb[0] == 0xE8:
                out['refresh_search_path'] = t + 5 + rel32(tb, 1)
    return out


if __name__ == '__main__':
    im = Image(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE)
    r = resolve(im)
    for k, v in r.items():
        print(f'{k:22s} {hex(v) if v else "NOT FOUND"}')
    sys.exit(0 if all(r.values()) else 1)
