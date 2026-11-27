#!/bin/bash
# Build the simulator and run the scenarios in scen*.c against the kernel
# built in the repo root (make first). Each scenario gets a fresh copy of
# os_image.bin as its hard disk.
#   tests/sim/run.sh             all of them
#   tests/sim/run.sh setup zipui only these
# Exit code: the number of scenarios that failed.
cd "$(dirname "$0")"
ROOT=../..
[ -f $ROOT/os_image.bin ] || { echo "build the OS first (make)"; exit 1; }
make -s -j"$(nproc)" sim gputest atapitest hdtest >/dev/null || exit 1
sh fixtures/make.sh 2>/dev/null || { echo "fixtures failed"; exit 1; }
mkdir -p out

# not tests: measurements and pictures
SKIP=" tlbcost readme readme320 bootscreen showcase "
# these check every frame against a full redraw ("+": verify mode)
VERIFY=" lazy lazylow "

# and some again in the bigger screens ("name@mode", "@modex2" = 2x zoom: sim.c)
SCREENS="desktop@640x480 desktop@800 alttab@800 winpos@800 pattern@800
         setup@640x480 setup@800 busycur@800 zipui@800 lazy@640x480 lazy@800
         zoom@320 zoom@800 zoom@640x2 zoom@640x480x2 zoom@800x2 desktop@800x2
         alttab@800x2 zipui@640x2 setup@800x2 pattern@640x2 lazy@800x2
         charset@320 help@320 joystick@320 desktop@320x16 desktop@640x256
         desktop@640x480x256 desktop@800x256 zoom@800x256 setup@800x256
         bmpview@640x256 bmpview@320 drawbmp@640x256 drawbmp@320
         drawtools@640x256 drawtools@320 drawsel@320 drawsel@640x256 asksave asksave@320
         clockfaces clockfaces@320 multisector
         scrapbook@640x256 memreport@320 memreport@640x256"

if [ $# -gt 0 ]; then list="$*"; else
    list="$(grep -ho '!strcmp(name, "[a-zA-Z0-9_]*")' scen*.c | sed 's/.*"\(.*\)".*/\1/' | sort -u) $SCREENS"
fi

failed=0; n=0; bad=""

# the kernel's own drivers/gpu.c on a model of the VGA planes
if [ $# -eq 0 ]; then
    n=1
    if ./gputest >out/gputest.txt 2>&1; then echo "ok    gputest"; else
        failed=1; bad=" gputest"; echo "FAIL  gputest"; grep FAIL out/gputest.txt | head -5 | sed 's/^/      /'
    fi
    # its drivers/ata.c (the CD-ROM part) on a model ATAPI drive
    n=2
    if ./atapitest >out/atapitest.txt 2>&1; then echo "ok    atapitest"; else
        failed=1; bad="$bad atapitest"; echo "FAIL  atapitest"; grep FAIL out/atapitest.txt | head -5 | sed 's/^/      /'
    fi
    # and on a model IDE hard disk (several sectors per command)
    n=3
    if ./hdtest >out/hdtest.txt 2>&1; then echo "ok    hdtest"; else
        failed=1; bad="$bad hdtest"; echo "FAIL  hdtest"; grep FAIL out/hdtest.txt | head -5 | sed 's/^/      /'
    fi
fi
for t in $list; do
    case "$SKIP" in *" $t "*) continue ;; esac
    arg=$t
    case "$VERIFY" in *" ${t%@*} "*) arg="+$t" ;; esac
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
