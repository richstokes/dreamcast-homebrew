# Source this to set MKDCDISC: the environment's, else the local build from
# tools/mkdcdisc/build-macos.sh, else one on the PATH.
if [[ -z "${MKDCDISC:-}" ]]; then
    MKDCDISC="${HOME}/.local/share/dreamcast/mkdcdisc/mkdcdisc"
    if [[ ! -x "${MKDCDISC}" ]]; then
        MKDCDISC="$(command -v mkdcdisc || true)"
    fi
fi
if [[ -z "${MKDCDISC}" || ! -x "${MKDCDISC}" ]]; then
    echo "mkdcdisc not found: run tools/mkdcdisc/build-macos.sh (or set MKDCDISC)" >&2
    return 1 2>/dev/null || exit 1
fi
export MKDCDISC
