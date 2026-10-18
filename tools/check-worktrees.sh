#!/usr/bin/env bash
#
# check-worktrees.sh - find work that has not reached main.
#
#   tools/check-worktrees.sh [--no-fetch] [base]
#
# Fetches origin (unless --no-fetch), then reports, against base (default
# origin/main):
#   - each worktree with uncommitted or untracked files, or commits not on base
#   - local and origin branches with commits not on base
#   - stashes
#   - a main checkout that is behind or ahead of origin/main
#
# A commit counts as on base when base has it or an equivalent patch (so a
# branch whose commits were rebased onto main is not reported). Exits 0 when
# everything is on base, 1 when something needs attention.

set -u

fetch=1
base=origin/main
for arg in "$@"; do
    case "$arg" in
        --no-fetch) fetch=0 ;;
        -h|--help) sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) base="$arg" ;;
    esac
done

repo="$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "not in a git repository" >&2; exit 2; }
cd "$repo" || exit 2
if (( fetch )); then
    git fetch -q --all --prune || echo "warning: fetch failed; comparing with the last fetched $base" >&2
fi
git rev-parse -q --verify "$base^{commit}" >/dev/null || { echo "unknown base $base" >&2; exit 2; }

problems=0
flag() { echo "  ! $*"; problems=$(( problems + 1 )); }

# Commits on $1 with no equivalent on base.
unmerged() { git cherry "$base" "$1" 2>/dev/null | grep -c '^+'; }

echo "Worktrees (against $base):"
report="$(mktemp -t check-worktrees)"
git worktree list --porcelain | awk '/^worktree /{sub(/^worktree /, ""); print}' | while IFS= read -r wt; do
    if [[ ! -d "$wt" ]]; then
        echo "  ! $wt: missing on disk (git worktree prune clears it)"
        continue
    fi
    head="$(git -C "$wt" rev-parse --abbrev-ref HEAD 2>/dev/null)"
    dirty="$(git -C "$wt" status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
    ahead="$(unmerged "$(git -C "$wt" rev-parse HEAD)")"
    line="$wt [$head]"
    if (( dirty || ahead )); then
        echo "  ! $line: $dirty uncommitted file(s), $ahead commit(s) not on $base"
    else
        echo "    $line: clean, on $base"
    fi
done | tee "$report"
grep -q '^  !' "$report" && problems=$(( problems + 1 ))
rm -f "$report"

echo "Branches with commits not on $base:"
found=0
while IFS= read -r ref; do
    [[ "$ref" == "$base" || "$ref" == origin/HEAD || "$ref" == origin ]] && continue
    n="$(unmerged "$ref")"
    if (( n )); then
        flag "$ref: $n commit(s), last $(git log -1 --format='%cs %s' "$ref")"
        found=1
    fi
done < <(git for-each-ref --format='%(refname:short)' refs/heads refs/remotes/origin)
(( found )) || echo "    none"

echo "Stashes:"
if [[ -n "$(git stash list)" ]]; then
    git stash list --format='%gd %cs %gs' | while IFS= read -r s; do echo "  ! $s"; done
    problems=$(( problems + 1 ))
else
    echo "    none"
fi

if git rev-parse -q --verify refs/heads/main >/dev/null && git rev-parse -q --verify refs/remotes/origin/main >/dev/null; then
    read -r ahead behind < <(git rev-list --left-right --count main...origin/main)
    echo "Local main against origin/main: $ahead ahead, $behind behind"
    (( ahead )) && flag "local main has $ahead commit(s) not pushed"
    (( behind )) && echo "    (behind is fine for work; the main checkout just needs a pull)"
fi

if (( problems )); then
    echo "Something has not reached $base (see ! lines)."
    exit 1
fi
echo "Everything is on $base."
