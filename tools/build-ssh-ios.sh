#!/bin/bash
# Build dropbear (SSH server) and dash (a /bin/sh) for iOS arm64, ad-hoc signed,
# for tools/sweet-add-ssh.py. Run on macOS (the GitHub macos-15 runner).
#
#   tools/build-ssh-ios.sh OUTDIR
#
# Output: OUTDIR/dropbear, OUTDIR/dash (Mach-O arm64, iOS 16, SHA-256 ad-hoc
# signature: the SWEET kernel accepts any validly signed binary on the root
# filesystem as a platform binary, see sweet_amfi.c).
#
# UNTESTED on the runner: if a step fails, the log shows which (configure,
# make, codesign). Sources are downloaded from the upstream sites.
set -euo pipefail

DROPBEAR_VER=${DROPBEAR_VER:-2024.86}
DASH_VER=${DASH_VER:-0.5.12}
MIN_IOS=16.0

[[ $# -eq 1 ]] || { sed -n '2,12p' "$0"; exit 2; }
mkdir -p "$1"
out=$(cd "$1" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc_real=$(xcrun --sdk iphoneos --find clang)
echo "SDK: $sdk"

# The iOS SDK marks fork()/system()/... as unavailable. Shadow the few headers
# involved with copies without the markers (-isystem is searched before the
# sysroot; stdlib.h pulls in _stdlib.h, where system() is declared).
overlay="$work/overlay"
mkdir -p "$overlay"
for h in unistd.h _unistd.h stdlib.h _stdlib.h spawn.h; do
	[[ -f "$sdk/usr/include/$h" ]] || continue
	sed -E 's/__IOS_PROHIBITED//g; s/__TVOS_PROHIBITED//g; s/__WATCHOS_PROHIBITED//g;
	        s/__API_UNAVAILABLE\([^)]*ios[^)]*\)//g; s/__OS_AVAILABILITY\(ios, *unavailable\)//g' \
		"$sdk/usr/include/$h" >"$overlay/$h"
done

CC="$cc_real -arch arm64 -isysroot $sdk -miphoneos-version-min=$MIN_IOS -isystem $overlay"
CFLAGS="-O2 -Wno-implicit-function-declaration -Wno-int-conversion"

# fork() must compile for the target, or nothing below can work.
printf '#include <unistd.h>\nint main(void){return fork();}\n' >"$work/t.c"
$CC $CFLAGS -c "$work/t.c" -o "$work/t.o" || {
	echo "error: the iOS SDK headers still reject fork()" >&2
	exit 1
}
# system() is not needed by dropbear's server path; report, do not stop.
printf '#include <stdlib.h>\nint main(void){return system("true");}\n' >"$work/t2.c"
$CC $CFLAGS -c "$work/t2.c" -o "$work/t2.o" || {
	echo "warning: system() still unavailable; the declaration is:" >&2
	grep -n -B4 'int system' "$overlay/_stdlib.h" "$sdk/usr/include/_stdlib.h" >&2 || true
}

fetch() { # URL... DEST : first working URL wins
	local dest=${!#} u
	for u in "${@:1:$#-1}"; do
		if curl -fsSL --retry 3 -o "$dest" "$u"; then return 0; fi
	done
	echo "download failed: $*" >&2
	return 1
}

# The SDK (iOS 18.x) declares functions that iOS 16 does not have (strchrnul,
# ...) as weak imports: configure finds them, the code calls them, and on the
# phone the call goes to NULL (SIGSEGV pc=0, seen in the first dash). A binary
# with weak imports is therefore refused here, with the list.
check_weak() {
	local w
	w=$(nm -mu "$1" 2>/dev/null | grep -i 'weak' || true)
	if [[ -n $w ]]; then
		echo "error: $1 has weak imports (NULL on iOS 16 => crash):" >&2
		echo "$w" >&2
		exit 1
	fi
	echo "ok: no weak imports in $(basename "$1")"
}

sign() {
	codesign --force -s - --digest-algorithm=sha256 --identifier "org.sweet.$(basename "$1")" "$1" ||
		codesign --force -s - "$1"
	codesign -dv "$1" 2>&1 | grep -E 'Identifier|Format|CodeDirectory' || true
}

# ---------------- dropbear ----------------
cd "$work"
fetch "https://matt.ucc.asn.au/dropbear/releases/dropbear-$DROPBEAR_VER.tar.bz2" \
	"https://dropbear.nl/mirror/releases/dropbear-$DROPBEAR_VER.tar.bz2" \
	"https://github.com/mkj/dropbear/archive/refs/tags/DROPBEAR_$DROPBEAR_VER.tar.gz" dropbear.tar
mkdir dropbear-src
tar xf dropbear.tar -C dropbear-src --strip-components=1
cd dropbear-src

# Public-key logins only (root has no usable password on iOS), no forwarding
# or agent, iOS-style PATH. This avoids crypt()/utmp/zlib entirely.
cat >localoptions.h <<'EOF'
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_CLI_PASSWORD_AUTH 0
#define DO_HOST_LOOKUP 0
#define DEFAULT_PATH "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
#define DEFAULT_ROOT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
EOF
./configure --host=aarch64-apple-darwin --enable-bundled-libtom \
	--disable-zlib --disable-pam --disable-lastlog --disable-utmp --disable-utmpx \
	--disable-wtmp --disable-wtmpx --disable-loginfunc --disable-pututline \
	--disable-pututxline \
	CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="-Wl,-dead_strip" \
	ac_cv_func_strchrnul=no ac_cv_func_mempcpy=no ac_cv_func_memmem=no
make -j"$(sysctl -n hw.ncpu)" PROGRAMS="dropbear"
cp dropbear "$out/dropbear"
check_weak "$out/dropbear"
strip -x "$out/dropbear" || true
sign "$out/dropbear"

# ---------------- dash ----------------
cd "$work"
fetch "http://gondor.apana.org.au/~herbert/dash/files/dash-$DASH_VER.tar.gz" \
	"https://git.kernel.org/pub/scm/utils/dash/dash.git/snapshot/dash-$DASH_VER.tar.gz" dash.tar.gz
mkdir dash-src
tar xf dash.tar.gz -C dash-src --strip-components=1
cd dash-src
[[ -x configure ]] || ./autogen.sh
# the generators (mkinit, mknodes, ...) run on the build machine
./configure --host=aarch64-apple-darwin --build="$(./config.guess 2>/dev/null || echo arm64-apple-darwin)" \
	CC="$CC" CFLAGS="$CFLAGS" CC_FOR_BUILD="clang" CFLAGS_FOR_BUILD="-O1" \
	LDFLAGS="-Wl,-dead_strip" \
	ac_cv_func_strchrnul=no ac_cv_func_mempcpy=no ac_cv_func_memmem=no
make -j"$(sysctl -n hw.ncpu)"
cp src/dash "$out/dash"
check_weak "$out/dash"
strip -x "$out/dash" || true
sign "$out/dash"

file "$out"/dropbear "$out"/dash
ls -l "$out"
