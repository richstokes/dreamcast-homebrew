# Sourced by project launchers. An explicit FLYCAST_BIN always wins.
if [[ -z "${FLYCAST_BIN:-}" ]]; then
    FLYCAST_BIN="${HOME}/.local/share/dreamcast/flycast/Flycast.app/Contents/MacOS/Flycast"
    if [[ ! -x "$FLYCAST_BIN" ]]; then
        FLYCAST_BIN="/Applications/Flycast.app/Contents/MacOS/Flycast"
    fi
fi
