#!/bin/sh

set -eu

workspace=/workspace
kos_base="$workspace/.ci/kos"
kos_ports="$workspace/.ci/kos-ports"

cp "$kos_base/doc/environ.sh.sample" "$kos_base/environ.sh"
sed -i "s|^export KOS_BASE=.*|export KOS_BASE=\"$kos_base\"|" "$kos_base/environ.sh"
sed -i "s|^export KOS_PORTS=.*|export KOS_PORTS=\"$kos_ports\"|" "$kos_base/environ.sh"

# shellcheck disable=SC1091
. "$kos_base/environ.sh"

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')

make -C "$kos_base" -j"$jobs"

# Dreamcast Browser is the only project with kos-ports dependencies. Building
# them here also proves the checked-in compatibility patch still applies.
make -C "$kos_ports/zlib" install
make -C "$kos_ports/mbedtls" force-install
make -C "$kos_ports/curl" install
make -C "$kos_ports/stb_image" install

for project in \
    demon-bazooka \
    dreamcast-browser \
    dreamcast-irc \
    gravity-wave \
    ping-cube
do
    make -C "$workspace/projects/$project" clean
    make -C "$workspace/projects/$project" -j"$jobs"
done
