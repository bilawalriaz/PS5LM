#!/usr/bin/env bash
# Do the patches apply? Run this after you edit one.
#
#   scripts/check-patches.sh
#
# Every patch in patches/ is applied to a throwaway worktree of the pinned
# llama.cpp submodule, in name order, exactly as scripts/build-app.sh does. A
# hand-edited patch is easy to leave with a stale hunk count, and git apply then
# refuses the whole series at build time (or, worse, in someone else's checkout).
#
# The worktree is removed on exit. Nothing in the submodule is touched.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
llama="$root/third_party/llama.cpp"
work="${TMPDIR:-/tmp}/ps5lm-patchcheck-$$"

[ -f "$llama/CMakeLists.txt" ] || { echo "check-patches: run git submodule update --init first" >&2; exit 1; }

git -C "$llama" worktree add --detach "$work" HEAD > /dev/null
trap 'git -C "'"$llama"'" worktree remove --force "'"$work"'" > /dev/null 2>&1 || true' EXIT

status=0
for p in "$root"/patches/*.patch; do
    [ -e "$p" ] || continue
    if git -C "$work" apply --check "$p" 2> /tmp/check-patches.err; then
        git -C "$work" apply "$p"
        echo "ok      $(basename "$p")"
    else
        echo "FAILED  $(basename "$p")"
        sed 's/^/        /' /tmp/check-patches.err
        status=1
    fi
done
rm -f /tmp/check-patches.err

if [ "$status" = 0 ]; then
    echo "check-patches: the series applies to the pinned submodule"
fi
exit "$status"
