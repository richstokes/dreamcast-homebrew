#!/bin/bash
#
# Update this repo's homebrew on a GDEMU SD card set up for openMenu.
#
# Finds the card (a volume with "dreamcast" in its name), reads each game
# folder's name.txt, and swaps any disc.cdi that belongs to a project here
# for the latest build from the rolling "continuous" GitHub release. Folders
# stay where they are, so openMenu's list needs no rebuild. Everything else
# on the card is left alone, and the card is ejected after an update.
# macOS only.
#
#   ./easy-SD-update.sh                 find the card, show the plan, ask, update
#   ./easy-SD-update.sh -n              show what would change and stop
#   ./easy-SD-update.sh -y /Volumes/DC  no questions, this card
#
set -euo pipefail

REPO=richstokes/dreamcast-homebrew
RELEASE_URL="https://github.com/$REPO/releases/download/continuous"
PROJECTS_URL="https://raw.githubusercontent.com/$REPO/main/.github/console-projects.txt"
PART=.disc.cdi.easy-sd-update

usage() {
    cat <<EOF
Usage: ./easy-SD-update.sh [-n] [-y] [-j JOBS] [/Volumes/CARD]

Updates this repo's homebrew discs on a GDEMU/openMenu SD card to the
latest builds from https://github.com/$REPO/releases/tag/continuous

  -n        dry run: show what would be updated, change nothing
  -y        don't ask before updating
  -j JOBS   discs to download and copy at once (default 4)
  -h        show this help

Without a path, the card is the mounted volume with "dreamcast" in its name.
EOF
}

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

dry_run=0
assume_yes=0
jobs=4
while getopts "nyj:h" opt; do
    case $opt in
        n) dry_run=1 ;;
        y) assume_yes=1 ;;
        j) jobs=$OPTARG ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ "$#" -le 1 ] || { usage >&2; exit 2; }
case $jobs in ''|*[!0-9]*|0) die "-j needs a positive number" ;; esac
[ "$(uname -s)" = Darwin ] || die "this script is for macOS"

interactive=0
[ -t 0 ] && [ "$assume_yes" -eq 0 ] && interactive=1

ask() {  # ask "question" -> 0 for yes (the default)
    local reply
    [ "$interactive" -eq 1 ] || return 0
    read -r -p "$1 [Y/n] " reply
    case $reply in [nN]*) return 1 ;; *) return 0 ;; esac
}

# --- Find the card ---------------------------------------------------------

card=${1:-}
if [ -z "$card" ]; then
    shopt -s nocasematch
    candidates=()
    for volume in /Volumes/*/; do
        volume=${volume%/}
        [[ $(basename "$volume") == *dreamcast* ]] && candidates+=("$volume")
    done
    shopt -u nocasematch
    case ${#candidates[@]} in
        0) die "no mounted volume has \"dreamcast\" in its name. Pass the card's path, e.g. ./easy-SD-update.sh /Volumes/SDCARD" ;;
        1) card=${candidates[0]} ;;
        *)
            [ "$interactive" -eq 1 ] || die "several cards found (${candidates[*]}); pass the one to update"
            echo "Several cards found:"
            select card in "${candidates[@]}"; do [ -n "$card" ] && break; done
            ;;
    esac
fi
card=${card%/}
[ -d "$card" ] || die "$card is not a directory"
[ "$dry_run" -eq 1 ] || [ -w "$card" ] || die "$card is read-only (is the card's lock switch on?)"

work=$(mktemp -d -t easy-sd-update)
cleanup() {
    rm -rf "$work"
    find "$card" -maxdepth 2 -name "$PART" -delete 2>/dev/null || true
}
trap cleanup EXIT
trap 'echo; echo "Interrupted; discs that were not finished keep their old version."; exit 130' INT TERM

# --- What the release has ----------------------------------------------------

curl -fsSL --retry 3 -o "$work/SHA256SUMS" "$RELEASE_URL/SHA256SUMS" ||
    die "couldn't fetch the release checksums (offline, or the release is mid-publish? try again in a minute)"
curl -fsSL --retry 3 -o "$work/projects.txt" "$PROJECTS_URL" ||
    die "couldn't fetch the project list from GitHub"

norm() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr -cd '[:alnum:]'; }

# slug|title|sha256 for every project the release has a CDI for.
while IFS='|' read -r _ slug title; do
    [ -n "$slug" ] || continue
    sum=$(awk -v f="dist/$slug.cdi" '$2 == f { print $1 }' "$work/SHA256SUMS")
    [ -n "$sum" ] && printf '%s|%s|%s\n' "$slug" "$title" "$sum"
done < "$work/projects.txt" > "$work/known.txt"
[ -s "$work/known.txt" ] || die "the release lists no discs; nothing to compare against"

# --- What the card has -------------------------------------------------------

# folder|slug|title|sha256 for each card folder holding one of our discs.
: > "$work/matches.txt"
for dir in "$card"/*/; do
    dir=${dir%/}
    [ -f "$dir/disc.cdi" ] || continue
    name=""
    for f in name.txt title.txt; do
        if [ -f "$dir/$f" ]; then
            name=$(head -n 1 "$dir/$f" | tr -d '\r')
            break
        fi
    done
    [ -n "$name" ] || continue
    key=$(norm "$name")
    while IFS='|' read -r slug title sum; do
        if [ "$key" = "$(norm "$title")" ] || [ "$key" = "$(norm "$slug")" ]; then
            printf '%s|%s|%s|%s\n' "$(basename "$dir")" "$slug" "$name" "$sum" >> "$work/matches.txt"
            break
        fi
    done < "$work/known.txt"
done

if [ ! -s "$work/matches.txt" ]; then
    echo "No discs from $REPO found on $card."
    exit 0
fi

# Hash what's on the card, a few discs at a time.
check_one() {  # folder|slug|name|sum -> same line with " ok" or " old"
    local folder slug name sum have
    IFS='|' read -r folder slug name sum <<< "$1"
    have=$(shasum -a 256 "$CARD/$folder/disc.cdi" | cut -d' ' -f1)
    if [ "$have" = "$sum" ]; then echo "$1|ok"; else echo "$1|old"; fi
}
export -f check_one
export CARD="$card"

echo "Checking $(wc -l < "$work/matches.txt" | tr -d ' ') discs on $card ..."
tr '\n' '\0' < "$work/matches.txt" |
    xargs -0 -n 1 -P "$jobs" bash -c 'check_one "$1"' _ |
    sort > "$work/status.txt"

echo
outdated=0
while IFS='|' read -r folder slug name sum state; do
    if [ "$state" = ok ]; then
        printf '  %-4s %-24s up to date\n' "$folder" "$name"
    else
        printf '  %-4s %-24s update available\n' "$folder" "$name"
        outdated=$((outdated + 1))
    fi
done < "$work/status.txt"
echo

if [ "$outdated" -eq 0 ]; then
    echo "Everything is up to date."
    exit 0
fi
if [ "$dry_run" -eq 1 ]; then
    echo "Dry run: $outdated disc(s) would be updated."
    exit 0
fi
ask "Update $outdated disc(s)?" || exit 0

# --- Download and copy -------------------------------------------------------

# One job per release file, so a game in two folders downloads once.
# Records are slug|name|sum|folder/folder/...
awk -F'|' '$5 == "old" {
    key = $2
    if (!(key in folders)) { order[++n] = key; head[key] = $2 "|" $3 "|" $4 }
    folders[key] = (key in folders) ? folders[key] "/" $1 : $1
} END { for (i = 1; i <= n; i++) print head[order[i]] "|" folders[order[i]] }' \
    "$work/status.txt" > "$work/jobs.txt"

update_one() {
    local slug name sum folders start file got mb folder dest offset date
    IFS='|' read -r slug name sum folders <<< "$1"
    start=$SECONDS
    file="$WORK/$slug.cdi"

    if ! curl -fsSL --retry 3 --retry-delay 2 -o "$file" "$RELEASE_URL/$slug.cdi"; then
        echo "  ✗ $name: download failed"
        return 1
    fi
    got=$(shasum -a 256 "$file" | cut -d' ' -f1)
    if [ "$got" != "$sum" ]; then
        echo "  ✗ $name: download doesn't match the release checksum (release republished mid-run? try again)"
        rm -f "$file"
        return 1
    fi
    mb=$(( $(stat -f %z "$file") / 1048576 ))

    # The disc's IP.BIN date, for the card manager's date.txt.
    date=""
    offset=$(grep -m 1 -oba 'SEGA SEGAKATANA' "$file" | cut -d: -f1 || true)
    if [ -n "$offset" ]; then
        date=$(dd if="$file" bs=1 skip=$((offset + 0x50)) count=8 2>/dev/null)
        [[ $date =~ ^[0-9]{8}$ ]] || date=""
    fi

    IFS=/ read -r -a folder_list <<< "$folders"
    for folder in "${folder_list[@]}"; do
        dest="$CARD/$folder"
        # Copy beside the old disc, then rename over it: an interrupted copy
        # leaves the old disc playable rather than half a new one.
        if ! cp -X "$file" "$dest/$PART" || ! mv -f "$dest/$PART" "$dest/disc.cdi"; then
            rm -f "$dest/$PART"
            echo "  ✗ $name: copy to $folder failed (card full or removed?)"
            return 1
        fi
        [ -n "$date" ] && [ -f "$dest/date.txt" ] && printf '%s' "$date" > "$dest/date.txt"
    done
    rm -f "$file"
    echo "  ✓ $name → ${folders//\// and } (${mb} MB, $((SECONDS - start))s)"
}
export -f update_one
export WORK="$work" RELEASE_URL PART

echo "Downloading and copying, $jobs at a time ..."
failed=0
tr '\n' '\0' < "$work/jobs.txt" |
    xargs -0 -n 1 -P "$jobs" bash -c 'update_one "$1"' _ || failed=1
sync

echo
if [ "$failed" -ne 0 ]; then
    echo "Some discs didn't update; run the script again to retry them."
    exit 1
fi
if diskutil eject "$card" >/dev/null 2>&1; then
    echo "Done. Card ejected; safe to remove."
else
    echo "Done, but $card couldn't be ejected (is a window or app using it?). Eject it before removing it."
fi
