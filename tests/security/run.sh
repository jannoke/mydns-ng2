#!/bin/sh
#
# Build and run the MyDNS security regression harnesses under AddressSanitizer +
# UndefinedBehaviorSanitizer.
#
# Usage:  tests/security/run.sh [BUILD_DIR]
#
# BUILD_DIR is a tree that has already been bootstrapped, configured and built with
# `make` (default: the repository root). Each harness #includes the real source file
# under test, so the code being checked is always the shipped code, compiled here
# with sanitizers; everything else is linked from BUILD_DIR's objects/libraries.
#
# A harness that #includes one of the server's own src/mydns/*.c files says so with a
#     /* HARNESS-REPLACES: foo.o */
# line; that object (and main.o) is then left out and the rest of src/mydns/*.o linked in.
#
# Harnesses call the parsers in-process with hand-built buffers - no network traffic.
# Exit 0 if every check passes, non-zero otherwise.

set -u
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
B=$(CDPATH= cd -- "${1:-$HERE/../..}" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

[ -f "$B/config.h" ] && [ -f "$B/src/lib/libmydns.a" ] || {
    echo "$B is not a built tree (run ./bootstrap.sh && ./configure && make first)"; exit 2; }

CC=${CC:-gcc}
mkvar() { sed -n "s/^$1 = //p" "$B/src/mydns/Makefile" | head -1; }
SQLINCLUDE=$(mkvar SQLINCLUDE)
LIBSQL=$(mkvar LIBSQL)

INC="-I$HERE -I$B -I$B/lib -I$B/src/lib -I$B/src/mydns -I$B/intl $SQLINCLUDE"
SAN="-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
DEFS="-funsigned-char -DHAVE_CONFIG_H -Wno-pointer-sign -Wno-format"
LIBS="$B/src/lib/libmydns.a $B/lib/libmydnsutil.a $LIBSQL -lm"

mydns_objs() {  # mydns_objs <object-to-leave-out>
    for o in "$B"/src/mydns/*.o; do
        case "$(basename "$o")" in main.o|"$1") ;; *) printf '%s ' "$o" ;; esac
    done
}

fail=0
names=""
echo "== building harnesses ($CC, tree: $B) =="
for src in "$HERE"/harness_*.c; do
    name=$(basename "$src" .c); name=${name#harness_}
    repl=$(sed -n 's/.*HARNESS-REPLACES: *\([A-Za-z0-9_]*\.o\).*/\1/p' "$src" | head -1)
    extra=""
    [ -n "$repl" ] && extra="$(mydns_objs "$repl") $HERE/server_stubs.c"
    # shellcheck disable=SC2086
    if $CC $SAN $DEFS $INC "$src" "$HERE/common.c" $extra $LIBS \
            -o "$WORK/$name" 2>"$WORK/$name.log"; then
        names="$names $name"
    else
        echo "BUILD FAIL: $name"; sed 's/^/    /' "$WORK/$name.log" | head -40; fail=1
    fi
done
[ "$fail" -eq 0 ] || { echo "== aborting: build errors =="; exit 1; }

echo
echo "== running checks =="
export ASAN_OPTIONS=detect_leaks=0
export UBSAN_OPTIONS=print_stacktrace=1
for name in $names; do
    # A harness that hangs (e.g. a decoder loop) is a failure, not a pass.
    timeout 20 "$WORK/$name"; rc=$?
    [ "$rc" -eq 124 ] && echo "FAIL $name: timed out (infinite loop?)"
    [ "$rc" -ne 0 ] && fail=1
done

echo
if [ "$fail" -eq 0 ]; then echo "== ALL SECURITY REGRESSION CHECKS PASSED =="; else echo "== FAILURES ABOVE =="; fi
exit $fail
