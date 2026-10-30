#!/bin/sh

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 /path/to/mkdcdisc /path/to/output-directory" >&2
    exit 2
fi

mkdcdisc=$1
output_dir=$2
# The MR logo on the BIOS license screen, the same one elf-to-cdi.sh puts on
# local discs.
logo="$(dirname "$0")/../../tools/mkdcdisc/logos/gotta-go-fast.png"

mkdir -p "$output_dir"

package() {
    directory=$1
    slug=$2
    title=$3

    elf="$directory/$slug.elf"
    # A project with a disc/ directory reads assets from the disc: those
    # files go in the image. One with a cdda/ directory plays its music as
    # CD-DA (DreamKarts): each WAV there becomes an audio track, in name
    # order. Neither publishes an ELF, since the ELF alone would lack them.
    disc_elf="$elf"
    disc_args=""
    publish_elf=1
    if [ -d "$directory/disc" ]; then
        disc_args="--directory-contents $directory/disc"
        publish_elf=0
    fi
    if [ -d "$directory/cdda" ]; then
        for track in "$directory"/cdda/*.wav; do
            disc_args="$disc_args --cdda-track $track"
        done
        publish_elf=0
    fi
    cdi="$output_dir/$slug.cdi"
    cdr_cdi="$output_dir/$slug-cdr.cdi"
    cdr_zip="$output_dir/$slug-cdr.zip"
    release_elf="$output_dir/$slug.elf"

    if [ ! -f "$elf" ]; then
        echo "missing release executable: $elf" >&2
        exit 1
    fi

    rm -f "$cdi" "$cdr_cdi" "$cdr_zip" "$release_elf"

    # Unpadded: a small image for emulators and GDEMU.
    # shellcheck disable=SC2086
    "$mkdcdisc" \
        --quiet \
        --disable-data-track-padding \
        --main-elf "$disc_elf" \
        $disc_args \
        --title "$title" \
        --author richstokes \
        --bootstrap-mr-image "$logo" \
        --output "$cdi"

    # Padded to a full 74-minute disc for burning. Unpadded, the whole boot
    # binary sits on the innermost tracks of the CD-R, where a Dreamcast's
    # CAV drive reads slowest and least reliably; burnt discs of the unpadded
    # image were reported hanging at the Dreamcast logo. The padding is ~720 MB
    # of filler that zips back down to roughly the size of the small image.
    # shellcheck disable=SC2086
    "$mkdcdisc" \
        --quiet \
        --main-elf "$disc_elf" \
        $disc_args \
        --title "$title" \
        --author richstokes \
        --bootstrap-mr-image "$logo" \
        --output "$cdr_cdi"
    (cd "$output_dir" && zip -q -9 "$slug-cdr.zip" "$slug-cdr.cdi")
    rm -f "$cdr_cdi"

    if [ "$publish_elf" = 1 ]; then
        cp "$elf" "$release_elf"
    fi

    echo "packaged $slug"
}

# Each project spends ~12 s in single-threaded mkdcdisc and zip runs, so
# package one per CPU at a time. pids holds "slug:pid" for each one running.
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')
pids=""
running=0
failed=0

wait_oldest() {
    # shellcheck disable=SC2086
    set -- $pids
    oldest=$1
    shift
    pids="$*"
    running=$((running - 1))
    if ! wait "${oldest#*:}"; then
        echo "packaging ${oldest%%:*} failed" >&2
        failed=1
    fi
}

while IFS='|' read -r directory slug title; do
    [ -n "$slug" ] || continue
    package "$directory" "$slug" "$title" < /dev/null &
    pids="$pids $slug:$!"
    running=$((running + 1))
    if [ "$running" -ge "$jobs" ]; then
        wait_oldest
    fi
done < .github/console-projects.txt

while [ "$running" -gt 0 ]; do
    wait_oldest
done

exit "$failed"
