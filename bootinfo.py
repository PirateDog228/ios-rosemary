#!/usr/bin/env python3
"""Print the header of an Android boot.img and what its kernel looks like.
Usage: python3 bootinfo.py boot.img"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
print('file size', len(d))
if d[:8] != b'ANDROID!':
    sys.exit('not an ANDROID! boot image: first bytes %r' % d[:16])
(ksz, kaddr, rsz, raddr, ssz, saddr, taddr, page, ver, osver) = struct.unpack('<10I', d[8:48])
print('header_version', ver, 'page', page, 'os_version %#x' % osver)
print('kernel  size %d addr %#x' % (ksz, kaddr))
print('ramdisk size %d addr %#x' % (rsz, raddr))
print('second  size %d addr %#x   tags addr %#x' % (ssz, saddr, taddr))
print('name   ', d[48:64].rstrip(b'\0'))
print('cmdline', (d[64:576] + d[608:1632]).rstrip(b'\0'))
if ver >= 1:
    rdsz, rdoff, hsz = struct.unpack('<IQI', d[1632:1648])
    print('recovery_dtbo size', rdsz, 'off', rdoff, 'header_size', hsz)
if ver >= 2:
    dsz, daddr = struct.unpack('<IQ', d[1648:1660])
    print('dtb     size %d addr %#x' % (dsz, daddr))
pg = lambda n: (n + page - 1) // page * page
k = d[page:page + ksz]
print('kernel first 64 bytes:', k[:64].hex())
print('kernel looks like:',
      'MTK header (0x58881688)' if k[:4] == b'\x88\x16\x88\x58' else
      'gzip' if k[:2] == b'\x1f\x8b' else
      'raw arm64 Image' if k[56:60] == b'ARMd' else 'unknown')
if k[:4] == b'\x88\x16\x88\x58':
    print('  MTK hdr size', struct.unpack('<I', k[4:8])[0], 'name', k[8:40].rstrip(b'\0'))
print('last 64 bytes of file (AVB footer is at the end of the partition, not file):', d[-64:].hex())
print('"AVBf" in file:', b'AVBf' in d)
