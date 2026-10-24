#!/usr/bin/env bash
# Build the rosemary shim and wrap it in a MediaTek Note 10S boot.img.
# Usage: OUT=out-rshim DTB=path/to/stock.dtb ./build_rosemary.sh [SWEETXNU-payload]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${OUT:-$HERE/out-rosemary}"
DTB="${DTB:-$ROOT/dtb/boot_unpacked/dtb}"

[[ -f "$DTB" ]] || { echo "DTB not found: $DTB (set DTB=...)" >&2; exit 1; }
[[ ! -e "$OUT" ]] || { echo "Output exists: $OUT (choose a new OUT=...)" >&2; exit 1; }

mkdir -p "$OUT"
CC="${CC:-aarch64-linux-gnu-gcc}"
OBJCOPY="${OBJCOPY:-aarch64-linux-gnu-objcopy}"
CFLAGS=(-O2 -ffreestanding -nostdlib -fno-pic -fno-builtin -mgeneral-regs-only
        -mstrict-align -fno-stack-protector -Wall -Wextra -DROSEMARY)

for f in start.S shim.c console.c mmu.c fdt.c loader.c; do
	"$CC" "${CFLAGS[@]}" -c "$HERE/$f" -o "$OUT/${f%.*}.o"
done

"$CC" "${CFLAGS[@]}" -static -T "$HERE/shim.ld" -Wl,--build-id=none \
	-Wl,--no-warn-rwx-segments -Wl,-N \
	"$OUT"/start.o "$OUT"/shim.o "$OUT"/console.o "$OUT"/mmu.o \
	"$OUT"/fdt.o "$OUT"/loader.o -o "$OUT/shim.elf"
"$OBJCOPY" -O binary "$OUT/shim.elf" "$OUT/Image"
size=$("${NM:-aarch64-linux-gnu-nm}" "$OUT/shim.elf" | awk '$3=="_end"{print $1}')
[[ -n "$size" ]] || { echo "Could not find _end in shim ELF" >&2; exit 1; }
truncate -s "$((16#$size))" "$OUT/Image"
gzip -9 -n -c "$OUT/Image" > "$OUT/Image.gz"

args=(--kernel "$OUT/Image.gz" --dtb "$DTB" -o "$OUT/rosemary-shim-boot.img")
if [[ $# -gt 0 ]]; then
	[[ -f "$1" ]] || { echo "Payload not found: $1" >&2; exit 1; }
	args+=(--ramdisk "$1")
fi
python3 "$ROOT/tools/mkboot_rosemary.py" "${args[@]}"
ls -l "$OUT/Image" "$OUT/Image.gz" "$OUT/rosemary-shim-boot.img"
