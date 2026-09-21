#!/bin/sh
# Runs every case in cases/ through the host build and compares the transcript
# (program output, typed input, queued tones, final error) with its .expected
# file. UPDATE=1 rewrites the expectations instead.
set -eu
cd "$(dirname "$0")"
rm -rf out
mkdir -p out
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
    case "$name" in app*) continue ;; esac
    program="programs/$name.bas"
    [ -f "$program" ] || program="../examples/$name.bas"
    ./host-basic run "$program" --keys "$keys" --tones > "out/$name.txt" 2>&1 || true
    compare "$name"
done

# The editor, driven by keys. Each transcript ends with the programs it saved.
for keys in cases/app*.keys; do
    name=$(basename "$keys" .keys)
    mkdir -p "out/store-$name"
    ./host-basic app --keys "$keys" --store "out/store-$name" \
        --examples ../examples > "out/$name.txt" 2>&1 || true
    for saved in "out/store-$name"/*.BAS; do
        [ -f "$saved" ] || continue
        echo "--- saved $(basename "$saved")" >> "out/$name.txt"
        cat "$saved" >> "out/$name.txt"
    done
    compare "$name"
done

exit $failed
