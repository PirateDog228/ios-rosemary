#!/usr/bin/env python3
"""Replace ONLY the kernel inside a stock Android boot.img (header v2); keep the
header fields (os_version, cmdline, addresses, name), the ramdisk, the dtb and the
AVB footer exactly as in the stock image. Bisect tool: if LK still rejects the
result, the kernel (size/format/header) is the problem, not the rest of the image.

Usage: repack_kernel.py stock_boot.img kernel[.gz] out.img [--cmdline-append 'text']
"""
import hashlib, struct, sys

stock = open(sys.argv[1], 'rb').read()
kernel = open(sys.argv[2], 'rb').read()
footer = stock[-64:]
assert footer[:4] == b'AVBf', 'no AVB footer at end of stock image'
original_image_size, vbmeta_offset, vbmeta_size = struct.unpack_from('>QQQ', footer, 12)
assert vbmeta_offset == original_image_size, 'unexpected AVB footer layout'
assert vbmeta_offset + vbmeta_size <= len(stock) - len(footer), 'invalid embedded vbmeta bounds'
vbmeta = stock[vbmeta_offset:vbmeta_offset + vbmeta_size]
if vbmeta_size:
    assert vbmeta[:4] == b'AVB0', 'AVB footer points to missing/invalid vbmeta'
assert stock[:8] == b'ANDROID!'
ksz, kaddr, rsz, raddr, ssz, saddr, taddr, page, ver, osver = struct.unpack('<10I', stock[8:48])
assert ver == 2, 'header v2 expected'
rdsz = struct.unpack('<I', stock[1632:1636])[0]
dsz = struct.unpack('<I', stock[1648:1652])[0]
pg = lambda n: -(-n // page) * page
off_k = page
off_r = off_k + pg(ksz)
off_s = off_r + pg(rsz)
off_rd = off_s + pg(ssz)
off_d = off_rd + pg(rdsz)
ramdisk = stock[off_r:off_r + rsz]
second = stock[off_s:off_s + ssz]
rdtbo = stock[off_rd:off_rd + rdsz]
dtb = stock[off_d:off_d + dsz]

hdr = bytearray(stock[:page])
if len(sys.argv) > 5 and sys.argv[4] == '--cmdline-append':
    cmd = (stock[64:576] + stock[608:1632]).rstrip(b'\0') + b' ' + sys.argv[5].encode()
    assert len(cmd) < 512 + 1024
    hdr[64:576] = cmd[:511].ljust(512, b'\0')
    hdr[608:1632] = cmd[511:].ljust(1024, b'\0')
struct.pack_into('<I', hdr, 8, len(kernel))
sha = hashlib.sha1()
for part in (kernel, ramdisk, second):
    sha.update(part); sha.update(struct.pack('<I', len(part)))
sha.update(struct.pack('<I', rdsz)); sha.update(rdtbo)
sha.update(dtb); sha.update(struct.pack('<I', len(dtb)))
hdr[576:608] = (sha.digest() + b'\0' * 12)[:32]

img = bytes(hdr) + kernel.ljust(pg(len(kernel)), b'\0') + ramdisk.ljust(pg(rsz), b'\0') \
    + second.ljust(pg(ssz), b'\0') + rdtbo.ljust(pg(rdsz), b'\0') + dtb.ljust(pg(dsz), b'\0')
assert len(img) <= vbmeta_offset, 'repacked boot payload overlaps embedded vbmeta'
img = img.ljust(vbmeta_offset, b'\0') + vbmeta
img = img.ljust(len(stock) - len(footer), b'\0') + footer
assert len(img) == len(stock)
open(sys.argv[3], 'wb').write(img)
print(f'{sys.argv[3]}: kernel {ksz} -> {len(kernel)} bytes, ramdisk/dtb/header/footer kept, total {len(img)}')
