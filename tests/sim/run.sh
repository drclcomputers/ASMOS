#!/bin/bash
# Build the simulator and run the scenarios in scen.c against the kernel
# built in the repo root (make first). Each scenario gets a fresh copy of
# os_image.bin as its hard disk.
#   tests/sim/run.sh             all of them
#   tests/sim/run.sh setup zipui only these
# Exit code: the number of scenarios that failed.
cd "$(dirname "$0")"
ROOT=../..
[ -f $ROOT/os_image.bin ] || { echo "build the OS first (make)"; exit 1; }
make -s -j"$(nproc)" >/dev/null || exit 1
sh fixtures/make.sh 2>/dev/null || { echo "fixtures failed"; exit 1; }
mkdir -p out

# not tests: measurements and pictures
SKIP=" tlbcost readme readme320 bootscreen showcase "
# these check every frame against a full redraw ("+": verify mode)
VERIFY=" lazy lazylow "

if [ $# -gt 0 ]; then list="$*"; else
    list=$(grep -o '!strcmp(name, "[a-zA-Z0-9_]*")' scen.c | sed 's/.*"\(.*\)".*/\1/' | sort -u)
fi

failed=0; n=0; bad=""
for t in $list; do
    case "$SKIP" in *" $t "*) continue ;; esac
    arg=$t
    case "$VERIFY" in *" $t "*) arg="+$t" ;; esac
    cp $ROOT/os_image.bin disk.img
    out=$(timeout 300 ./sim "$arg" 2>&1); rc=$?
    n=$((n + 1))
    fails=$(printf '%s\n' "$out" | grep -E " FAIL$|MISMATCH|^missing: " )
    if [ $rc -ne 0 ] || [ -n "$fails" ]; then
        failed=$((failed + 1)); bad="$bad $t"
        echo "FAIL  $t (exit $rc)"
        printf '%s\n' "$fails" | head -5 | sed 's/^/      /'
    else
        echo "ok    $t"
    fi
done
rm -f disk.img
echo
echo "$((n - failed)) of $n scenarios passed${bad:+; failed:$bad}"
exit $failed
