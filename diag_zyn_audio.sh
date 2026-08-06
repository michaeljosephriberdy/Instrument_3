#!/usr/bin/env bash
#
# diag_zyn_audio.sh
#
# Run this WHILE the instrument is running (Zyn should already be launched).
# It doesn't change anything -- just reports what PipeWire actually sees,
# so we can tell which stage of the chain is broken:
#   process running? -> ports exist? -> links exist? -> sink correct? -> not muted?
set -uo pipefail

echo "=== 1. Is zynaddsubfx actually running? ==="
pgrep -af zynaddsubfx || echo "  NOT RUNNING"
echo ""

echo "=== 2. Zyn's PipeWire/JACK ports (outputs) ==="
pw-link -o 2>/dev/null | grep -i zyn || echo "  no zyn output ports found"
echo ""

echo "=== 3. Zyn's PipeWire/JACK ports (inputs, incl. MIDI) ==="
pw-link -i 2>/dev/null | grep -i zyn || echo "  no zyn input ports found"
echo ""

echo "=== 4. All current playback (speaker/headphone) input ports ==="
pw-link -i 2>/dev/null | grep -i playback
echo ""

echo "=== 5. Active links involving zyn ==="
pw-link -l 2>/dev/null | grep -i zyn || echo "  NO LINKS involving zyn -- pw-link calls are failing/silent"
echo ""

echo "=== 6. Your engine's MIDI output port (what launchZyn's code expects to link FROM) ==="
pw-link -o 2>/dev/null | grep -iv zyn | grep -i midi
echo "  (compare this against cfg_.engine_midi_name in your config/instrument.json --"
echo "   if the exact string doesn't match one of the lines above, pw-link silently fails)"
echo ""

echo "=== 7. Default sink / volume / mute state ==="
if command -v wpctl >/dev/null; then
    wpctl status 2>/dev/null | sed -n '/Sinks:/,/Sources:/p'
    echo "---"
    DEFAULT_SINK_ID=$(wpctl status 2>/dev/null | grep -A5 'Sinks:' | grep '\*' | grep -oP '\d+(?=\.)')
    if [[ -n "${DEFAULT_SINK_ID:-}" ]]; then
        wpctl get-volume "$DEFAULT_SINK_ID" 2>/dev/null
    fi
else
    echo "  wpctl not found -- install pipewire tools"
fi
echo ""

echo "=== 8. Instrument's own log tail (last 40 lines, if present) ==="
if [[ -f "./instrument.log" ]]; then
    tail -n 40 ./instrument.log
else
    echo "  ./instrument.log not found in current dir"
fi
