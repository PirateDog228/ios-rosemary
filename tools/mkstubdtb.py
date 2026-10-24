#!/usr/bin/env python3
"""Tiny FDT writer for host tests of the rosemary diag parser (NOT for the phone:
use the DTB from your own stock boot.img there)."""
import struct, sys

def build(root):
    strs = bytearray(); offs = {}
    def sname(n):
        if n not in offs:
            offs[n] = len(strs); strs.extend(n.encode() + b'\0')
        return offs[n]
    st = bytearray()
    def pad(): st.extend(b'\0' * (-len(st) % 4))
    def emit(name, props, kids):
        st.extend(struct.pack('>I', 1)); st.extend(name.encode() + b'\0'); pad()
        for k, v in props:
            st.extend(struct.pack('>III', 3, len(v), sname(k))); st.extend(v); pad()
        for c in kids: emit(*c)
        st.extend(struct.pack('>I', 2))
    emit(*root); st.extend(struct.pack('>I', 9))
    off_st = 40 + 16; off_str = off_st + len(st); total = off_str + len(strs)
    hdr = struct.pack('>10I', 0xd00dfeed, total, off_st, off_str, 40, 17, 16, 0, len(strs), len(st))
    return hdr + b'\0' * 16 + bytes(st) + bytes(strs)

u32 = lambda *v: struct.pack('>%dI' % len(v), *v)
cs = lambda s: s.encode() + b'\0'

def stub():
    return build(('', [('#address-cells', u32(2)), ('#size-cells', u32(2)),
                       ('model', cs('MT6785 stub')), ('compatible', cs('mediatek,MT6785'))], [
        ('chosen', [('bootargs', cs('bootopt=64S3,32N2,64N2 rdiag_stage=1')),
                    ('atag,videolfb-fb_base_l', u32(0x7e605000)), ('atag,videolfb-fb_base_h', u32(0)),
                    ('atag,videolfb-vramSize', u32(0x17e8000)), ('atag,videolfb-lcmname', cs('stub_lcm')),
                    ('atag,videolfb-islcmfound', u32(1)), ('atag,videolfb-islcm_inited', u32(1))], []),
        ('memory@40000000', [('device_type', cs('memory')), ('reg', u32(0, 0x40000000, 1, 0x80000000))], []),
        ('cpus', [], [('cpu@0', [('reg', u32(0))], []), ('cpu@100', [('reg', u32(0x100))], [])]),
        ('reserved-memory', [], [('scp-reserved', [('reg', u32(0, 0x50000000, 0, 0x400000))], [])]),
    ]))

if __name__ == '__main__':
    open(sys.argv[1], 'wb').write(stub())
