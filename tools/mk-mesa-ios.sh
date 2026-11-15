#!/bin/bash
# Mesa 25.0.7 (OSMesa, OpenGL ES 2/3) with the panfrost Mali driver for iOS arm64, and the OpenGL ES shim
# libredwood-gl.dylib (userland/src/eagl.c) on top of it. For the GitHub macos-15 runner, from hakios'
# tools/mk-mesa.sh (its patches in tools/mesa are used as they are: Bifrost v7 only, which is the G52 and the G76).
#
#   tools/mk-mesa-ios.sh OUTDIR
#
# OUTDIR gets libOSMesa.8.dylib (install name /redwood/lib/libOSMesa.8.dylib), libredwood-gl.dylib
# (install name /redwood/lib/libredwood-gl.dylib), both ad-hoc signed, and gl.xml/exported-gl.txt.
set -euo pipefail

VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
MIN_IOS=16.0
DRIVERS=${DRIVERS:-softpipe,panfrost}

[[ $# -eq 1 ]] || { sed -n '2,9p' "$0"; exit 2; }
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${MESA_WORK:-$ROOT/mesa-work}
mkdir -p "$B"
cd "$B"

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CCREAL=$(xcrun --sdk iphoneos --find clang)
CXXREAL=$(xcrun --sdk iphoneos --find clang++)
echo "SDK: $SDK"

# tools: Apple's bison is 2.3 (Mesa needs 2.4+), so Homebrew's comes first in PATH
if command -v brew >/dev/null; then
	for t in meson ninja bison flex; do command -v $t >/dev/null || brew install $t; done
	BISON_PREFIX=$(brew --prefix bison 2>/dev/null || true)
	[[ -z $BISON_PREFIX ]] || export PATH="$BISON_PREFIX/bin:$PATH"
fi
bison --version | head -1

PATCHES=$(cat "$ROOT"/tools/mesa/*.patch | shasum -a 256 | cut -d' ' -f1)
if [[ ! -d mesa-$VERSION || "$(cat mesa-$VERSION/.redwood-patches 2>/dev/null)" != "$PATCHES" ]]; then
	[[ -f mesa-$VERSION.tar.xz ]] || curl -sL --fail --retry 3 -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	rm -rf mesa-$VERSION build-*
	tar xJf mesa-$VERSION.tar.xz
	for p in "$ROOT"/tools/mesa/*.patch; do
		echo "patch: $(basename "$p")"
		patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"
	done
	echo "$PATCHES" > mesa-$VERSION/.redwood-patches
fi
rm -rf mesa-$VERSION/src/redwood-drm
cp -r "$ROOT/tools/mesa/redwood-drm" mesa-$VERSION/src/redwood-drm
rm -rf mesa-$VERSION/src/compiler/air
cp -r "$ROOT/tools/mesa/air" mesa-$VERSION/src/compiler/air

if [[ ! -x venv/bin/python3 ]]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging
fi
export PATH="$B/venv/bin:$PATH"

cat > cross.ini <<INI
[binaries]
c = ['$CCREAL', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=$MIN_IOS']
cpp = ['$CXXREAL', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=$MIN_IOS', '-fuse-cxa-atexit']
objc = ['$CCREAL', '-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=$MIN_IOS']
ar = 'ar'
ranlib = 'ranlib'
strip = 'strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI

BUILD=build-${DRIVERS//,/-}
OPT=
case "$DRIVERS" in *panfrost*) OPT=-Doptimization=s ;; esac
[[ -f $BUILD/build.ninja ]] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/redwood --libdir=lib --buildtype=release $OPT -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false >meson-setup.log 2>&1 || { tail -60 meson-setup.log; exit 1; }
ninja -C $BUILD >ninja.log 2>&1 || { tail -80 ninja.log; exit 1; }

check_weak() {
	# pthread_mutexattr_*: in every iOS since 2.0; the SDK of Xcode 16 only marks them weak
	local w; w=$(nm -mu "$1" 2>/dev/null | grep -i 'weak' | grep -v -E '_pthread_mutexattr_(init|destroy|settype)( |$)' || true)
	if [[ -n $w ]]; then
		echo "error: $1 has weak imports (NULL on iOS 16 => crash):" >&2
		echo "$w" >&2
		exit 1
	fi
	echo "ok: no weak imports in $(basename "$1")"
}
sign() { codesign --force -s - --digest-algorithm=sha256 --identifier "org.sweet.$(basename "$1")" "$1" || codesign --force -s - "$1"; }

strip -x -o "$OUT/libOSMesa.8.dylib" $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
install_name_tool -id /redwood/lib/libOSMesa.8.dylib "$OUT/libOSMesa.8.dylib"
nm -gU "$OUT/libOSMesa.8.dylib" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > "$OUT/exported-gl.txt"
cp mesa-$VERSION/src/mapi/glapi/registry/gl.xml "$OUT/gl.xml"
check_weak "$OUT/libOSMesa.8.dylib"
sign "$OUT/libOSMesa.8.dylib"
echo "libOSMesa: $(wc -c < "$OUT/libOSMesa.8.dylib") bytes, $(wc -l < "$OUT/exported-gl.txt") gl* functions"

# ---- the OpenGL ES shim for CoreAnimation ----
G=$B/gl
rm -rf "$G"; mkdir -p "$G"
python3 "$ROOT/userland/mkeagl.py" "$ROOT/userland/src/eagl_slots.txt" "$OUT/gl.xml" "$OUT/exported-gl.txt" "$ROOT/userland/src/eagl.c" > "$G/eagl_gen.h"
"$CCREAL" -arch arm64 -isysroot "$SDK" -miphoneos-version-min=$MIN_IOS -O2 -dynamiclib \
	-install_name /redwood/lib/libredwood-gl.dylib -I"$G" -I"$ROOT/userland/src" \
	-framework CoreFoundation -framework IOKit -framework IOSurface -lobjc "$OUT/libOSMesa.8.dylib" \
	-Wno-deprecated-declarations -o "$OUT/libredwood-gl.dylib" "$ROOT/userland/src/eagl.c" "$ROOT/userland/src/gl_osmesa.c"
check_weak "$OUT/libredwood-gl.dylib"
sign "$OUT/libredwood-gl.dylib"
ls -l "$OUT"
