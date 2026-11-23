#!/bin/bash
# Build rosemary-inspectord for the arm64 iOS 16 userland. Run on macos-15.
set -euo pipefail
[[ $# -eq 1 ]] || { echo "Usage: $0 OUTDIR" >&2; exit 2; }

here=$(cd "$(dirname "$0")/.." && pwd)
out=$(mkdir -p "$1" && cd "$1" && pwd)
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos --find clang)

"$cc" \
  -arch arm64 \
  -isysroot "$sdk" \
  -miphoneos-version-min=16.0 \
  -std=c11 -O2 -Wall -Wextra -Werror=implicit-function-declaration \
  "$here/tools/inspector-agent/rosemary_inspectord.c" \
  -o "$out/rosemary-inspectord"

codesign --force -s - --digest-algorithm=sha256 \
  --identifier org.rosemary.inspector-daemon "$out/rosemary-inspectord"
file "$out/rosemary-inspectord"
shasum -a 256 "$out/rosemary-inspectord"
