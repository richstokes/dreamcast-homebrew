#!/bin/sh

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 /path/to/mkdcdisc /path/to/output-directory" >&2
    exit 2
fi

mkdcdisc=$1
output_dir=$2

mkdir -p "$output_dir"

while IFS='|' read -r slug title; do
    [ -n "$slug" ] || continue

    elf="projects/$slug/$slug.elf"
    cdi="$output_dir/$slug.cdi"
    release_elf="$output_dir/$slug.elf"

    if [ ! -f "$elf" ]; then
        echo "missing release executable: $elf" >&2
        exit 1
    fi

    rm -f "$cdi" "$release_elf"
    "$mkdcdisc" \
        --quiet \
        --disable-data-track-padding \
        --main-elf "$elf" \
        --title "$title" \
        --author richstokes \
        --output "$cdi"
    cp "$elf" "$release_elf"
done <<'PROJECTS'
demon-bazooka|Demon Bazooka
dreamcast-browser|Dreamcast Browser
dreamcast-irc|Dreamcast IRC
gravity-wave|Gravity Wave
ping-cube|Ping Cube
PROJECTS
