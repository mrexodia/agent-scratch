#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Fetch QEMU's x87 tests (tests/tcg/i386, GPL - downloaded, not part of this
# repository), build them natively and run each under tests/x87trace, which
# checks every x87 instruction they execute on this CPU against the emulator.
#
# usage: tests/trace-qemu.sh [workdir]        (x86-64 Linux, needs curl + cc)
set -e
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-${TMPDIR:-/tmp}/x87-qemu-tests}
base=https://raw.githubusercontent.com/qemu/qemu/master/tests/tcg/i386
tests="fxam fldcst pseudo-denormal snan-convert fisttp fbstp fscale fxtract fp-exceptions fyl2x fyl2xp1 f2xm1 fpatan fprem"

mkdir -p "$work"
status=0
for t in $tests; do
    src="$work/test-i386-$t.c"
    [ -f "$src" ] || curl -fsS -o "$src" "$base/test-i386-$t.c"
    ${CC:-cc} -O1 -static -no-pie -w "$src" -o "$work/test-i386-$t" -lm
    # fprem iterates for a very long time: only sample its beginning
    limit=0
    [ "$t" = fprem ] && limit=12000
    # keep the tracer's own exit status: a pipeline would report tail's
    if "$here/x87trace" -q -n $limit "$work/test-i386-$t" > "$work/$t.log" 2>&1; then
        tail -n 1 "$work/$t.log"
    else
        cat "$work/$t.log"
        echo "FAILED: test-i386-$t" >&2
        status=1
    fi
done
exit $status
