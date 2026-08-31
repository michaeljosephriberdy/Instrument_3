#!/usr/bin/env bash
# prep_for_pi5.sh
# Run once from the root of the microtonal-instrument repo, on the machine
# where it currently lives, BEFORE cloning onto the new Pi 5.
#
# What it does:
#   1. Adds/updates .gitignore for build/, *.bak* files, and backup dirs.
#   2. Untracks build/ from git (stale x86_64 CMakeCache + object files +
#      a nested json-src .git that clones badly) -- left on disk untouched.
#   3. Untracks the various *.bak / *_bak backup files and .mode6_bak_*,
#      .phase3_integration_bak dirs -- left on disk untouched.
#   4. Untracks instrument.log (a runtime-generated log, not source).
#
# It does NOT delete anything from your working directory, and it does NOT
# commit for you -- review with `git status` and commit yourself.
#
# Safe to run more than once (idempotent).

set -euo pipefail

if [ ! -d .git ] || [ ! -f CMakeLists.txt ]; then
  echo "ERROR: run this from the project root (where .git and CMakeLists.txt live)." >&2
  exit 1
fi

echo "=== 1. Writing/updating .gitignore ==="
GITIGNORE_ENTRIES=(
  "build/"
  "*.bak"
  "*.bak.*"
  "*_bak"
  "*_bak_*"
  ".mode6_bak_*/"
  ".mode6_fix_bak_*/"
  ".phase3_integration_bak/"
  "instrument.log"
)
touch .gitignore
for entry in "${GITIGNORE_ENTRIES[@]}"; do
  if ! grep -qxF "$entry" .gitignore; then
    echo "$entry" >> .gitignore
    echo "  added: $entry"
  fi
done

echo
echo "=== 2. Untracking build/ (kept on disk, just stops being versioned) ==="
if git ls-files --error-unmatch build >/dev/null 2>&1; then
  git rm -r --cached --ignore-unmatch build >/dev/null
  echo "  untracked build/"
else
  echo "  build/ not tracked, skipped"
fi

echo
echo "=== 3. Untracking stray *bak* files/dirs (kept on disk) ==="
mapfile -d '' -t bak_files < <(git ls-files -z | grep -zE '(\.|_)bak([._/]|$)' || true)
if [ "${#bak_files[@]}" -gt 0 ]; then
  git rm -r --cached --ignore-unmatch -- "${bak_files[@]}" >/dev/null
  printf '  untracked: %s\n' "${bak_files[@]}"
else
  echo "  none found"
fi

echo
echo "=== 4. Untracking instrument.log if tracked ==="
if git ls-files --error-unmatch instrument.log >/dev/null 2>&1; then
  git rm --cached --ignore-unmatch instrument.log >/dev/null
  echo "  untracked instrument.log"
else
  echo "  not tracked, skipped"
fi

echo
echo "=== Done ==="
echo "Review with: git status"
echo "Then commit:  git add -A && git commit -m 'Stop tracking build artifacts and backup files before Pi 5 deploy'"
echo
echo "Reminder for when the Pi 5 arrives (can't be scripted ahead of time):"
echo "  Run pi_audio_diagnose.sh ON THE PI 5 to get its real PipeWire port"
echo "  names, then build with:"
echo "    sudo ./build.sh --headless-boot --audio-left '<name>' --audio-right '<name>'"
echo "  The hardcoded fallback sink name in audio_graph_manager.cpp is this"
echo "  machine's onboard audio and won't exist on the Pi 5."
