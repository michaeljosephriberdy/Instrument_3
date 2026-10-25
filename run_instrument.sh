#!/usr/bin/env bash
# run_instrument.sh -- start the instrument together with a selectable "Instrument Mic".
#
# Run from the project's root directory:   ./run_instrument.sh
# Any arguments are passed on to the instrument.
#
# What it does, every launch:
#   1. removes any "Instrument Mic" left over from an earlier run,
#   2. creates a fresh one (an audio source Firefox/Cheese can select),
#   3. keeps instrument-bus:out_1/out_2 linked into it (re-linking if the bus restarts)
#      and makes sure nothing else (e.g. your normal mic) is linked into it,
#   4. runs the instrument in the foreground exactly as before,
#   5. when the instrument exits (Ctrl-C, closing the terminal) removes the mic again and
#      puts your previous default microphone back.
# The mic can never stop the instrument from starting: any problem is only a warning.
#
# Environment switches:
#   INSTRUMENT_MIC=0          do not create the mic at all
#   INSTRUMENT_MIC_DEFAULT=0  create it, but do not make it the system default microphone
#   INSTRUMENT_BIN=path       run a different binary (default build/microtonal_instrument)
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${INSTRUMENT_BIN:-$REPO/build/microtonal_instrument}"
MIC_ENABLE="${INSTRUMENT_MIC:-1}"
MIC_SET_DEFAULT="${INSTRUMENT_MIC_DEFAULT:-1}"
BUS="instrument-bus"
MIC_NODE="instrument_mic"
MIC_IN="instrument_mic_in"
MIC_LOG="${XDG_RUNTIME_DIR:-/tmp}/instrument_mic.log"
OLD_MIC_REGEX='pw-loopback -n instrument_mic( |$)'

LOOP_PID=""
WATCH_PID=""
PREV_DEFAULT=""

say() { printf '[instrument-mic] %s\n' "$*" >&2; }

remove_old_mic() {
  pkill -f "$OLD_MIC_REGEX" 2>/dev/null || true
  local i
  for i in $(seq 1 30); do
    pw-link -i 2>/dev/null | grep -q "^${MIC_IN}:" || return 0
    sleep 0.1
  done
  say "warning: an old Instrument Mic is still listed; continuing anyway"
}

start_mic() {
  PREV_DEFAULT="$(pactl get-default-source 2>/dev/null || true)"
  [ "$PREV_DEFAULT" = "$MIC_NODE" ] && PREV_DEFAULT=""
  pw-loopback -n "$MIC_NODE" -m '[ FL FR ]' \
    --capture-props="node.name=$MIC_IN node.autoconnect=false node.description=\"Instrument Mic (in)\"" \
    --playback-props="media.class=Audio/Source node.name=$MIC_NODE node.description=\"Instrument Mic\"" \
    >"$MIC_LOG" 2>&1 </dev/null &
  LOOP_PID=$!
}

link_once() {
  pw-link "$BUS:out_1" "$MIC_IN:input_FL" >/dev/null 2>&1 || true
  pw-link "$BUS:out_2" "$MIC_IN:input_FR" >/dev/null 2>&1 || true
}

# Remove anything linked into the mic's inputs that is not the instrument bus.
unlink_strays() {
  pw-link -l 2>/dev/null | awk -v want="$BUS:" -v pfx="${MIC_IN}:input_" '
    /^[^ \t]/ { cur = $0; next }
    index(cur, pfx) == 1 && match($0, /^[ \t]+\|<- /) {
      src = substr($0, RLENGTH + 1)
      if (index(src, want) != 1) print src "\t" cur
    }
  ' | while IFS=$'\t' read -r src dst; do
        pw-link -d "$src" "$dst" >/dev/null 2>&1 || true
      done
}

watcher() {
  local parent="$1" default_done=0
  while kill -0 "$parent" 2>/dev/null; do
    if kill -0 "$LOOP_PID" 2>/dev/null; then
      link_once
      unlink_strays
      if [ "$MIC_SET_DEFAULT" = "1" ] && [ "$default_done" = "0" ]; then
        pactl set-default-source "$MIC_NODE" >/dev/null 2>&1 && default_done=1
      fi
    fi
    sleep 1
  done
  # the launcher is gone without cleaning up (e.g. killed): do not leave the mic behind
  kill "$LOOP_PID" 2>/dev/null || true
}

cleanup() {
  trap - EXIT INT TERM HUP
  [ -n "$WATCH_PID" ] && kill "$WATCH_PID" 2>/dev/null
  [ -n "$LOOP_PID" ] && kill "$LOOP_PID" 2>/dev/null
  if [ -n "$PREV_DEFAULT" ] && [ "$MIC_SET_DEFAULT" = "1" ]; then
    if [ "$(pactl get-default-source 2>/dev/null || true)" = "$MIC_NODE" ]; then
      pactl set-default-source "$PREV_DEFAULT" >/dev/null 2>&1 || true
    fi
  fi
}

if [ ! -x "$BIN" ]; then
  say "cannot run $BIN (missing or not executable) -- build first with ./build.sh"
  exit 1
fi
cd "$REPO" || exit 1

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

if [ "$MIC_ENABLE" = "1" ]; then
  if command -v pw-loopback >/dev/null 2>&1 && command -v pw-link >/dev/null 2>&1 \
     && command -v pactl >/dev/null 2>&1; then
    remove_old_mic
    start_mic
    watcher "$$" </dev/null >/dev/null 2>&1 &
    WATCH_PID=$!
    say "Instrument Mic created (pick \"Instrument Mic\" in Firefox/Cheese)"
  else
    say "warning: pw-loopback, pw-link or pactl not found; starting without the Instrument Mic"
  fi
fi

"$BIN" "$@"
rc=$?
exit "$rc"
