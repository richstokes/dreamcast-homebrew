#!/bin/sh

set -eu

# Builds every project in the release manifest against the SDK that
# build-sdk-in-container.sh built (or CI restored from its cache).

workspace=/workspace
kos_base="$workspace/.ci/kos"

# The workspace is owned by the host runner, while this container runs as
# root. DreamKarts puts the workspace commit on its title screen.
git config --global --add safe.directory "$workspace"

# KOS probes optional variables while its environment is sourced, so nounset
# must be temporarily disabled (as it is for local builds in AGENTS.md).
set +u
# shellcheck disable=SC1091
. "$kos_base/environ.sh"
set -u

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')

make -f "$workspace/.github/scripts/build-projects.mk" \
    -j"$jobs" --output-sync=recurse WORKSPACE="$workspace"
