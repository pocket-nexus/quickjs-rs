#!/usr/bin/env bash
# libquickjs-sys/prebuilt/build-psp.sh — compile this crate's QuickJS for the
# PSP into libquickjs-mipsel-sony-psp.a, through the crate's own build.rs and
# the flags a PSP host build passes, and record every input beside it.
#
#   LLVM_BIN=<dir with clang and llvm-ar> PSP_SDK=<extracted mipsel-sony-psp dir> \
#     libquickjs-sys/prebuilt/build-psp.sh <output dir>
#
# psp.env pins the Rust toolchain and the LLVM major version;
# LLVM_MAJOR_OVERRIDE replaces the pinned major for a local build, and the JSON
# records the clang that ran.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
crate=$(dirname "$here")
repo=$(dirname "$crate")
# shellcheck source=psp.env
source "$here/psp.env"
LLVM_MAJOR=${LLVM_MAJOR_OVERRIDE:-$LLVM_MAJOR}

out=${1:?usage: build-psp.sh <output dir>}
: "${LLVM_BIN:?LLVM_BIN names the directory holding clang and llvm-ar}"
: "${PSP_SDK:?PSP_SDK names the extracted mipsel-sony-psp directory}"
PSP_SDK=$(cd "$PSP_SDK" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
target_dir=$(mktemp -d)
trap 'rm -rf "$target_dir"' EXIT

clang_version=$("$LLVM_BIN/clang" --version | head -n1)
case "$clang_version" in
  *"version $LLVM_MAJOR."*) ;;
  *) echo "build-psp.sh: $LLVM_BIN/clang is \"$clang_version\"; LLVM $LLVM_MAJOR is required" >&2; exit 1 ;;
esac

# The flags of a PSP host build (pocketjs tools/psp.ts), plus prefix maps so
# the archive does not carry this machine's paths, and -nostdlibinc so the
# headers come from the PSP SDK and clang alone, not from the host's
# /usr/include (glibc's limits.h breaks the build on Linux).
cflags="-target mipsel-sony-psp -mcpu=mips2 -msingle-float -mlittle-endian -mno-abicalls -fno-pic -G0"
cflags="$cflags -mno-check-zero-division -fno-stack-protector -O2"
cflags="$cflags -ffile-prefix-map=$target_dir=/build -ffile-prefix-map=$PSP_SDK=/sdk"
cflags="$cflags -nostdlibinc -I$PSP_SDK/psp/include -I$PSP_SDK/psp/sdk/include"

(
  cd "$repo"
  CRATE_CC_NO_DEFAULTS=1 \
  TARGET_CC="$LLVM_BIN/clang" \
  TARGET_AR="$LLVM_BIN/llvm-ar" \
  TARGET_CFLAGS="$cflags" \
  PSPDEV="$PSP_SDK" \
  CARGO_TARGET_DIR="$target_dir" \
    rustup run "$RUST_TOOLCHAIN" cargo check -p libquickjs-sys \
      --target "$here/mipsel-sony-psp.json" -Z build-std=core -Z json-target-spec
)

lib=$(find "$target_dir" -path '*/build/libquickjs-sys-*/out/libquickjs.a' | head -n1)
[ -n "$lib" ] || { echo "build-psp.sh: build.rs produced no libquickjs.a" >&2; exit 1; }
archive="$out/libquickjs-mipsel-sony-psp.a"
cp "$lib" "$archive"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
commit=$(git -C "$repo" rev-parse HEAD)
dirty=$(git -C "$repo" status --porcelain -- libquickjs-sys | grep -q . && echo true || echo false)
cat > "$out/libquickjs-mipsel-sony-psp.json" <<JSON
{
  "schema": "quickjs-prebuilt/1",
  "target": "mipsel-sony-psp",
  "archive": { "file": "libquickjs-mipsel-sony-psp.a", "sha256": "$(sha256 "$archive")", "bytes": $(wc -c < "$archive" | tr -d ' ') },
  "source": { "commit": "$commit", "dirty": $dirty, "quickjsVersion": "$(cat "$crate/embed/quickjs/VERSION")" },
  "toolchain": { "rust": "$RUST_TOOLCHAIN", "clang": "$clang_version" },
  "pspSdk": { "url": "$PSP_SDK_URL", "sha256": "$PSP_SDK_SHA256" },
  "cflags": "$(echo "$cflags" | sed -e "s#$target_dir#<target>#g" -e "s#$PSP_SDK#<sdk>#g")"
}
JSON
echo "build-psp.sh: $archive"
