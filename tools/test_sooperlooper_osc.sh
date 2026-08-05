#!/usr/bin/env bash
# Quick OSC smoke test against a running SooperLooper (port 9951).
set -euo pipefail
PORT="${1:-9951}"
if ! command -v oscsend >/dev/null; then
  echo "install liblo-tools (oscsend)"
  exit 1
fi
if ! command -v sooperlooper >/dev/null; then
  echo "sooperlooper not installed"
  exit 1
fi
echo "Sending test hits to localhost:$PORT ..."
oscsend localhost "$PORT" /sl/0/hit s record
sleep 0.3
oscsend localhost "$PORT" /sl/0/hit s overdub
sleep 0.3
oscsend localhost "$PORT" /sl/0/hit s pause
echo "Done. Watch sooperlooper UI/console for state changes."
