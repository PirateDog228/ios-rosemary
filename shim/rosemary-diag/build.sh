#!/usr/bin/env bash
# Build diagnostic kernels inside the stock Redmi Note 10S boot image.
# This preserves the stock ramdisk, DTB, header, and image size for boot_b tests.
set -euo pipefail

if [[ -z "${STOCK:-}" ]]; then
  echo "Set STOCK=path/to/stock-boot.img" >&2
  exit 1
fi
STOCK="$(realpath "$STOCK")"
[[ -f "$STOCK" ]] || { echo "Stock boot image not found: $STOCK" >&2; exit 1; }

cd "$(dirname "$0")"
OUT="${OUT:-out-rosemary-diag}"
if [[ -e "$OUT" ]]; then
  echo "Output already exists: $OUT (set OUT to a new directory; existing files are preserved)" >&2
  exit 1
fi
mkdir -p "$OUT"

if command -v aarch64-linux-gnu-gcc >/dev/null; then
  CC=(aarch64-linux-gnu-gcc)
  LD=(aarch64-linux-gnu-gcc -static -nostdlib -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N)
  OBJCOPY=aarch64-linux-gnu-objcopy
else
  CC=(clang --target=aarch64-linux-gnu)
  LD=(ld.lld --build-id=none -N)
  OBJCOPY=llvm-objcopy
fi
CFLAGS=(-O2 -ffreestanding -nostdlib -fno-pic -fno-builtin -mgeneral-regs-only -mstrict-align
        -fno-stack-protector -Wall -Wextra)
for f in start.S diag.c console.c fdt.c m2.c ufs_mtk.c; do
  "${CC[@]}" "${CFLAGS[@]}" -c "$f" -o "$OUT/${f%.*}.o"
done
"${LD[@]}" -T shim.ld "$OUT"/{start,diag,console,fdt,m2,ufs_mtk}.o -o "$OUT/rosemary-diag.elf"
"$OBJCOPY" -O binary "$OUT/rosemary-diag.elf" "$OUT/Image"
size=$(( 0x$(nm "$OUT/rosemary-diag.elf" | awk '$3=="_end"{print $1}') ))
truncate -s "$size" "$OUT/Image"
gzip -9 -n -c "$OUT/Image" > "$OUT/Image.gz"

STAGE="${STAGE:-0}"
case "$STAGE" in 0|1|2|3|4|5|6|7|8) ;; *) echo "STAGE must be 0-8" >&2; exit 1 ;; esac
python3 ../../tools/repack_kernel.py "$STOCK" "$OUT/Image.gz" "$OUT/stage$STAGE-boot.img" \
  --cmdline-append "rdiag_stage=$STAGE ${EXTRA_CMDLINE:-}"
ls -l "$OUT/stage$STAGE-boot.img"
