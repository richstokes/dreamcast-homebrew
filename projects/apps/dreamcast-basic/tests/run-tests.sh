#!/bin/sh
# Runs every case in cases/ through the host build and compares the transcript
# (program output, typed input, queued tones, final error) with its .expected
# file. UPDATE=1 rewrites the expectations instead.
set -eu
cd "$(dirname "$0")"
rm -rf out
mkdir -p out/store
failed=0
# The harness exits from inside a run once its script is used up.
export ASAN_OPTIONS=detect_leaks=0

compare() {
    name=$1
    if [ "${UPDATE:-0}" = 1 ]; then
        cp "out/$name.txt" "cases/$name.expected"
    elif ! diff -u "cases/$name.expected" "out/$name.txt" > "out/$name.diff"; then
        echo "FAIL $name"
        head -40 "out/$name.diff"
        failed=1
    else
        echo "ok   $name"
    fi
}

for keys in cases/*.keys; do
    name=$(basename "$keys" .keys)
    [ "$name" = app ] && continue
    program="programs/$name.bas"
    [ -f "$program" ] || program="../examples/$name.bas"
    ./host-basic run "$program" --keys "$keys" --tones > "out/$name.txt" 2>&1 || true
    compare "$name"
done

# The editor: type a program, save it, start over, load it back and run it.
./host-basic app --keys cases/app.keys --store out/store --examples ../examples \
    > out/app.txt 2>&1 || true
{ echo "--- saved file"; cat out/store/DEMO.BAS; } >> out/app.txt
compare app

exit $failed
