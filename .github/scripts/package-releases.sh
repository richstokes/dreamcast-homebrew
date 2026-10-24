#!/bin/sh

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 /path/to/mkdcdisc /path/to/output-directory" >&2
    exit 2
fi

mkdcdisc=$1
output_dir=$2

mkdir -p "$output_dir"

while IFS='|' read -r directory slug title; do
    [ -n "$slug" ] || continue

    elf="$directory/$slug.elf"
    # A project with a disc build streams assets from the disc instead of
    # linking them in: its CDIs come from $slug-disc.elf with disc/ added,
    # while the ELF asset stays the self-contained one.
    disc_elf="$elf"
    disc_args=""
    if [ -f "$directory/$slug-disc.elf" ]; then
        disc_elf="$directory/$slug-disc.elf"
    fi
    if [ -d "$directory/disc" ]; then
        disc_args="--directory-contents $directory/disc"
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
        --output "$cdr_cdi"
    (cd "$output_dir" && zip -q -9 "$slug-cdr.zip" "$slug-cdr.cdi")
    rm -f "$cdr_cdi"

    cp "$elf" "$release_elf"
done < .github/console-projects.txt
