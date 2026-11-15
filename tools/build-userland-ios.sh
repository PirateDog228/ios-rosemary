#!/bin/bash
# Build GPU userland for iOS arm64 (ad-hoc signed). Run on macos-15.  tools/build-userland-ios.sh OUTDIR
set -euo pipefail
[[ $# -eq 1 ]] || { sed -n '2,2p' "$0"; exit 2; }
mkdir -p "$1"; out=$(cd "$1" && pwd)
here=$(cd "$(dirname "$0")/.." && pwd)
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)
CC="$cc -arch arm64 -isysroot $sdk -miphoneos-version-min=16.0"
check_weak() {
	local w; w=$(nm -mu "$1" 2>/dev/null | grep -i weak || true)
	[[ -z $w ]] || { echo "error: weak imports in $1:" >&2; echo "$w" >&2; exit 1; }
	echo "ok: no weak imports in $(basename "$1")"
}
sign() { codesign --force -s - --digest-algorithm=sha256 --identifier "org.sweet.$(basename "$1")" "$1" || codesign --force -s - "$1"; }

# IOKit headers are not in the iPhoneOS SDK's public set on some versions: use the framework.
$CC -O2 -Wall "$here/userland/src/mali_test.c" -o "$out/mali_test" -framework IOKit -framework CoreFoundation
check_weak "$out/mali_test"
sign "$out/mali_test"

$CC -O2 -Wall "$here/userland/src/bbwrap.c" -o "$out/bbwrap"
check_weak "$out/bbwrap"
sign "$out/bbwrap"

$CC -O2 -Wall -dynamiclib -install_name /usr/local/lib/libbbload.dylib "$here/userland/src/bbload.c" -o "$out/libbbload.dylib" -lobjc
check_weak "$out/libbbload.dylib"
sign "$out/libbbload.dylib"

# empty injection test dylib: prints a line to the log when loaded into a process
cat >"$out/inject_test.c" <<'EOT'
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
__attribute__((constructor)) static void init(void)
{
	int fd = open("/tmp/inject_test.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) { char b[64]; int n = snprintf(b, sizeof b, "loaded in pid %d\n", getpid()); write(fd, b, n); close(fd); }
}
EOT
$CC -O2 -dynamiclib "$out/inject_test.c" -o "$out/inject_test.dylib"
rm "$out/inject_test.c"
check_weak "$out/inject_test.dylib"
sign "$out/inject_test.dylib"
file "$out"/*; ls -l "$out"
