#!/bin/bash
# Build a tiny (64 MiB) case-sensitive journaled HFS+ root whose /sbin/launchd is
# the bring-up test init (tools/sweet-init.S wrapped by tools/mkinit.py).
# It proves the whole storage path (UFS driver -> HFS+ mount -> exec of PID 1 ->
# console) without needing an IPSW. Linux only.
#
#   mktinyroot.sh LAUNCHD_MACHO OUT.img
#
# Needs: hfsprogs (mkfs.hfsplus, fsck.hfsplus) and sudo for the loop mount.
set -euo pipefail
[[ $# -eq 2 ]] || { sed -n '2,10p' "$0"; exit 2; }
init=$(realpath "$1") out=$2
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }
for t in mkfs.hfsplus fsck.hfsplus; do
	command -v "$t" >/dev/null || { echo "missing $t (apt install hfsprogs)" >&2; exit 1; }
done
truncate -s 64M "$out"
mkfs.hfsplus -s -J -v tinyroot "$out"
mnt=$(mktemp -d)
sudo mount -t hfsplus -o loop,force "$out" "$mnt"
trap 'sudo umount "$mnt" 2>/dev/null || true; rmdir "$mnt" 2>/dev/null || true' EXIT
sudo mkdir -p "$mnt/sbin" "$mnt/dev" "$mnt/private/var"
sudo install -m 755 -o root -g root "$init" "$mnt/sbin/launchd"
sudo umount "$mnt"
trap - EXIT
rmdir "$mnt"
# repair pass first (clears Linux-mount leftovers, same as README step 2), then verify
fsck.hfsplus -f -y "$out" || true
fsck.hfsplus -f -n "$out"
echo "ok: $out"
