#!/usr/bin/env bash
# blank_octave_buttons.sh
#
# Remove octave_up / octave_down bindings from the top-board layouts.
# Those keys become inert (action "none" or simply omitted).
# All other keys are left unchanged.
#
# Usage:
#   ./blank_octave_buttons.sh
#   ./blank_octave_buttons.sh /path/to/Instrument_3

set -euo pipefail

PROJECT_DIR="${1:-}"

find_project_dir() {
  local start="$1"
  [[ -f "$start/config/layouts/top_right.json" && \
     -f "$start/config/layouts/top_left.json" ]] || return 1
  echo "$start"
}

if [[ -n "$PROJECT_DIR" ]]; then
  PROJECT_DIR="$(cd "$PROJECT_DIR" && pwd)"
  find_project_dir "$PROJECT_DIR" >/dev/null || {
    echo "error: bad path (need config/layouts/top_{left,right}.json)" >&2
    exit 1
  }
else
  if PROJECT_DIR="$(find_project_dir "$(pwd)")"; then
    :
  else
    CANDIDATE=""
    for d in "$HOME"/*/ "$HOME"/*/*/ /home/*/*/ /home/*/*/*/; do
      [[ -f "${d}config/layouts/top_right.json" ]] || continue
      CANDIDATE="${d%/}"
      break
    done
    [[ -n "$CANDIDATE" ]] || {
      echo "error: project not found" >&2
      exit 1
    }
    PROJECT_DIR="$CANDIDATE"
  fi
fi

LAYOUTS="$PROJECT_DIR/config/layouts"
STAMP="$(date +%Y%m%d_%H%M%S)"

echo "Project: $PROJECT_DIR"

python3 - "$LAYOUTS" "$STAMP" <<'PY'
import json
import sys
from pathlib import Path

layouts_dir = Path(sys.argv[1])
stamp = sys.argv[2]

TARGETS = ("octave_up", "octave_down")

for name in ("top_right.json", "top_left.json"):
    path = layouts_dir / name
    if not path.is_file():
        print(f"skip (missing): {path}")
        continue

    bak = path.with_suffix(path.suffix + f".bak.{stamp}")
    bak.write_bytes(path.read_bytes())
    print(f"backed up -> {bak.name}")

    data = json.loads(path.read_text())
    keys = data.get("keys", [])
    if not isinstance(keys, list):
        print(f"error: {path} has no keys array", file=sys.stderr)
        sys.exit(1)

    removed = []
    kept = []
    for k in keys:
        action = str(k.get("action", "")).lower()
        if action in TARGETS:
            removed.append(f"row={k.get('row')} col={k.get('col')} {action}")
            # Drop the key entirely so it has no function.
            # (Alternatively we could set action="none"; omitting is cleaner.)
            continue
        kept.append(k)

    data["keys"] = kept
    path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"{name}: removed {len(removed)} octave key(s)")
    for r in removed:
        print(f"  - {r}")

print("done — restart the instrument to load the updated layouts")
PY
