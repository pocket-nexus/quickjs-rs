#!/bin/sh
# Builds the minor-collector tests with ASan + UBSan (QuickJS asserts on)
# and runs every scenario.
#
#   sh libquickjs-sys/gc-tests/run.sh [build-dir]
#
# If STOCK_REV names a commit of this repository without the minor
# collector, the stock-equivalence test is also built against that
# revision's QuickJS and both outputs must match byte for byte.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
src="$here/../embed/quickjs"
out=${1:-"${TMPDIR:-/tmp}/quickjs-gc-tests"}
cc=${CC:-cc}
mkdir -p "$out"

cflags="-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined
  -fno-sanitize-recover=undefined -D_GNU_SOURCE -DCONFIG_VERSION=\"test\"
  -funsigned-char -w"

build() { # build <output> <quickjs dir> <test source>
    # shellcheck disable=SC2086
    $cc $cflags -I"$2" -o "$1" "$3" "$2/quickjs.c" "$2/cutils.c" "$2/dtoa.c" \
        "$2/libregexp.c" "$2/libunicode.c" "$2/patch.c" -lm
}

build "$out/minor_gc_reentry" "$src" "$here/minor_gc_reentry.c"
build "$out/gc_stock_equivalence" "$src" "$here/gc_stock_equivalence.c"

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
fail=0
for sc in $("$out/minor_gc_reentry"); do
    if "$out/minor_gc_reentry" "$sc" >"$out/$sc.log" 2>&1; then
        echo "PASS $(cat "$out/$sc.log")"
    else
        echo "FAIL $sc (exit $?)"; tail -n 20 "$out/$sc.log"; fail=1
    fi
done

if "$out/gc_stock_equivalence" >"$out/equivalence.txt" 2>"$out/equivalence.log"; then
    echo "PASS gc_stock_equivalence ($(wc -l <"$out/equivalence.txt") lines)"
else
    echo "FAIL gc_stock_equivalence (exit $?)"; tail -n 20 "$out/equivalence.log"; fail=1
fi

if [ -n "${STOCK_REV:-}" ]; then
    stock="$out/stock-quickjs"
    rm -rf "$stock" && mkdir -p "$stock"
    top=$(git -C "$here" rev-parse --show-toplevel)
    git -C "$top" archive "$STOCK_REV" libquickjs-sys/embed/quickjs |
        tar -x -C "$stock" --strip-components=3
    build "$out/gc_stock_equivalence.stock" "$stock" "$here/gc_stock_equivalence.c"
    "$out/gc_stock_equivalence.stock" >"$out/equivalence.stock.txt"
    if cmp "$out/equivalence.txt" "$out/equivalence.stock.txt"; then
        echo "PASS gc_stock_equivalence matches $STOCK_REV"
    else
        echo "FAIL gc_stock_equivalence differs from $STOCK_REV"; fail=1
    fi
fi
exit $fail
