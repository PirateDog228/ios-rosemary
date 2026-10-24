#!/usr/bin/env python3
"""Pack a kernel Image into an Android boot.img (header v2) for the Redmi
Note 10S (rosemary, MT6785), with no dependency on AOSP mkbootimg.

The layout is the one Lineage uses for this phone (android-device/BoardConfig.mk):
  base 0x40078000, kernel +0x8000, ramdisk +0x07c08000, tags/dtb +0x0bc08000,
  page size 2048, header v2, cmdline "bootopt=64S3,32N2,64N2".
MediaTek LK loads the kernel to base+kernel_offset (0x40080000), picks the DTB
from the dtb slot, fixes it up (memory, /chosen, bootargs) and jumps to the
Image with x0 = DTB. The build script gzip-compresses the shim to match the
stock boot image's kernel format. The 512-byte MTK wrapper header is not needed.

Usage: mkboot_rosemary.py --kernel Image.gz --dtb mt6785.dtb [--ramdisk payload.bin] -o boot.img
"""
import argparse, gzip, hashlib, struct

BASE = 0x40078000
KERNEL_OFFSET = 0x00008000
RAMDISK_OFFSET = 0x07c08000
SECOND_OFFSET = 0xbff88000
TAGS_OFFSET = 0x0bc08000
DTB_OFFSET = 0x0bc08000
PAGE = 2048
CMDLINE = 'bootopt=64S3,32N2,64N2'


def empty_ramdisk():
    """gzip'd newc cpio holding only TRAILER!!! (LK dislikes a missing ramdisk)."""
    name = b'TRAILER!!!\0'
    hdr = b'070701' + b''.join(b'%08X' % v for v in
                               (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
    blob = hdr + name
    blob += b'\0' * (-len(blob) % 4)
    blob += b'\0' * (-len(blob) % 512)
    return gzip.compress(blob, 9, mtime=0)


def pad(b):
    return b + b'\0' * (-len(b) % PAGE)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--dtb', required=True)
    ap.add_argument('--ramdisk')
    ap.add_argument('--cmdline', default=CMDLINE)
    ap.add_argument('-o', '--output', required=True)
    a = ap.parse_args()

    kernel = open(a.kernel, 'rb').read()
    dtb = open(a.dtb, 'rb').read()
    ramdisk = open(a.ramdisk, 'rb').read() if a.ramdisk else empty_ramdisk()
    m32 = lambda v: (BASE + v) & 0xffffffff

    sha = hashlib.sha1()
    for part in (kernel, ramdisk, b''):
        sha.update(part)
        sha.update(struct.pack('<I', len(part)))
    sha.update(b'')
    sha.update(struct.pack('<I', 0))           # recovery_dtbo
    sha.update(dtb)
    sha.update(struct.pack('<I', len(dtb)))

    cmd = a.cmdline.encode()
    assert len(cmd) < 512 + 1024
    os_version = ((11 << 14) << 11) | ((2025 - 2000) << 4) | 1   # Android 11.0.0, 2025-01

    hdr = b'ANDROID!'
    hdr += struct.pack('<10I', len(kernel), m32(KERNEL_OFFSET), len(ramdisk),
                       m32(RAMDISK_OFFSET), 0, m32(SECOND_OFFSET), m32(TAGS_OFFSET),
                       PAGE, 2, os_version)
    hdr += b'rosemary-diag'.ljust(16, b'\0')
    hdr += cmd[:511].ljust(512, b'\0')
    hdr += (sha.digest() + b'\0' * 12)[:32]
    hdr += cmd[511:].ljust(1024, b'\0')
    hdr += struct.pack('<IQI', 0, 0, 1660)                       # recovery dtbo, header_size
    hdr += struct.pack('<IQ', len(dtb), BASE + DTB_OFFSET)       # dtb size/addr
    assert len(hdr) == 1660

    img = pad(hdr) + pad(kernel) + pad(ramdisk) + pad(dtb)
    open(a.output, 'wb').write(img)
    print(f'{a.output}: {len(img)} bytes, kernel@{m32(KERNEL_OFFSET):#x} '
          f'ramdisk@{m32(RAMDISK_OFFSET):#x} tags@{m32(TAGS_OFFSET):#x} dtb {len(dtb)}B')


if __name__ == '__main__':
    main()
