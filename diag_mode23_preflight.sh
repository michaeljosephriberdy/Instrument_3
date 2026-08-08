#!/usr/bin/env bash
# diag_mode23_preflight.sh
#
# Run BEFORE starting the instrument.
# Checks everything Mode 2 (VocoderOnly) and Mode 3 (SynthAndVocoder) need:
# packages, LV2 URI, mic, ability to host Calf under jalv, playback sinks.
# Prints PASS/FAIL per check and concrete fix commands when something is wrong.
#
# Usage:
#   ./diag_mode23_preflight.sh
#   ./diag_mode23_preflight.sh /path/to/Instrument_3   # optional; looks for instrument.log later

set -uo pipefail

MIC_HINT="${MIC_HINT:-MVX2U}"
VOCODER_URI="${VOCODER_URI:-http://calf.sourceforge.net/plugins/Vocoder}"
NODE_NAME="${NODE_NAME:-InstrumentVocoder}"
FAILS=0
WARNS=0

pass() { printf '  [PASS] %s\n' "$*"; }
fail() { printf '  [FAIL] %s\n' "$*"; FAILS=$((FAILS + 1)); }
warn() { printf '  [WARN] %s\n' "$*"; WARNS=$((WARNS + 1)); }
info() { printf '         %s\n' "$*"; }
fix()  { printf '  [FIX ] %s\n' "$*"; }
hdr()  { printf '\n=== %s ===\n' "$*"; }

have() { command -v "$1" >/dev/null 2>&1; }

hdr "0. Environment"
info "user=$(id -un)  host=$(hostname)  pwd=$(pwd)"
info "MIC_HINT=$MIC_HINT  VOCODER_URI=$VOCODER_URI  NODE_NAME=$NODE_NAME"
if have pw-link; then
  pass "pw-link available (PipeWire graph tools present)"
else
  fail "pw-link not found — PipeWire CLI tools missing"
  fix  "sudo apt install -y pipewire-bin pipewire-audio-client-libraries"
fi

# ---------------------------------------------------------------------------
hdr "1. Packages required for Mode 2 / 3"
# ---------------------------------------------------------------------------
NEED_BINS=(zynaddsubfx sooperlooper jalv lv2ls oscsend)
OPT_BINS=(jalv.gtk jalv.gtk3 pw-jack wpctl)
for b in "${NEED_BINS[@]}"; do
  if have "$b"; then
    pass "binary: $b -> $(command -v "$b")"
  else
    fail "binary missing: $b"
  fi
done
for b in "${OPT_BINS[@]}"; do
  if have "$b"; then
    pass "optional: $b -> $(command -v "$b")"
  else
    warn "optional missing: $b"
  fi
done

# apt package hints (best-effort)
if have dpkg; then
  for pkg in zynaddsubfx sooperlooper calf-plugins jalv liblo-tools \
             pipewire-jack pipewire-audio-client-libraries; do
    if dpkg -l "$pkg" 2>/dev/null | grep -qE '^ii'; then
      pass "package installed: $pkg"
    else
      fail "package not installed: $pkg"
    fi
  done
fi

if ! have jalv && ! have jalv.gtk && ! have jalv.gtk3; then
  fix "sudo apt install -y jalv calf-plugins"
fi
if ! have lv2ls; then
  fix "sudo apt install -y lilv-utils   # or calf-plugins meta that pulls lv2 tools"
fi
if ! have oscsend; then
  fix "sudo apt install -y liblo-tools"
fi
if ! have pw-jack; then
  fix "sudo apt install -y pipewire-jack"
  info "Without pw-jack, JACK clients (Zyn/SL/jalv) may not see the PipeWire graph."
fi

# ---------------------------------------------------------------------------
hdr "2. Calf Vocoder LV2 URI"
# ---------------------------------------------------------------------------
URI_FOUND=""
if have lv2ls; then
  LV2_OUT="$(lv2ls 2>/dev/null || true)"
  if echo "$LV2_OUT" | grep -qi 'Vocoder'; then
    URI_FOUND="$(echo "$LV2_OUT" | grep -i 'Vocoder' | head -n1)"
    pass "Vocoder URI visible in lv2ls:"
    info "$URI_FOUND"
    if echo "$LV2_OUT" | grep -qF "$VOCODER_URI"; then
      pass "exact configured URI present: $VOCODER_URI"
    else
      warn "configured URI not exact-matched; instrument still tries launch"
      info "configured: $VOCODER_URI"
      info "found:      $URI_FOUND"
      fix  "If launch fails, set calf_vocoder_uri in config or code to the found URI."
    fi
  else
    fail "no 'Vocoder' plugin in lv2ls output"
    fix  "sudo apt install -y calf-plugins && lv2ls | grep -i vocoder"
  fi
else
  fail "lv2ls missing — cannot verify LV2 install"
fi

# ---------------------------------------------------------------------------
hdr "3. Microphone (Shure $MIC_HINT) — ALSA + PipeWire"
# ---------------------------------------------------------------------------
MIC_ALSA_OK=0
MIC_PW_OK=0

if have arecord; then
  CARDS="$(arecord -l 2>/dev/null || true)"
  if echo "$CARDS" | grep -qi "$MIC_HINT"; then
    pass "ALSA sees $MIC_HINT"
    echo "$CARDS" | grep -i "$MIC_HINT" | while read -r line; do info "$line"; done
    MIC_ALSA_OK=1
  else
    fail "ALSA does not list $MIC_HINT"
    fix  "Plug in the Shure MVX2U USB interface, wait 2s, re-run this script."
    fix  "Check: lsusb | grep -i shure"
    info "arecord -l output (truncated):"
    echo "$CARDS" | head -n 20 | while read -r line; do info "$line"; done
  fi
else
  warn "arecord not found (alsa-utils)"
  fix  "sudo apt install -y alsa-utils"
fi

if have pw-link; then
  PW_IN="$(pw-link -i 2>/dev/null || true)"
  PW_OUT="$(pw-link -o 2>/dev/null || true)"
  MIC_IN="$(echo "$PW_IN" | grep -iE "$MIC_HINT|Shure" || true)"
  if [[ -n "$MIC_IN" ]]; then
    pass "PipeWire input ports matching mic:"
    echo "$MIC_IN" | while read -r line; do info "$line"; done
    MIC_PW_OK=1
    # Prefer non-monitor
    NON_MON="$(echo "$MIC_IN" | grep -vi monitor || true)"
    if [[ -z "$NON_MON" ]]; then
      warn "only monitor ports found — modulator/dry may be silent or wrong"
      fix  "Use capture/input ports, not monitor, if both appear after mode switch."
    else
      pass "non-monitor capture-style ports present"
    fi
  else
    fail "no PipeWire input ports matching $MIC_HINT / Shure"
    fix  "Unplug/replug MVX2U; ensure PipeWire is running: systemctl --user status pipewire"
    fix  "pw-cli list-objects | grep -i MVX2U"
  fi
  MIC_OUT="$(echo "$PW_OUT" | grep -iE "$MIC_HINT|Shure" || true)"
  if [[ -n "$MIC_OUT" ]]; then
    info "PipeWire output/monitor ports (informational):"
    echo "$MIC_OUT" | while read -r line; do info "$line"; done
  fi
fi

if [[ "$MIC_ALSA_OK" -eq 0 && "$MIC_PW_OK" -eq 0 ]]; then
  fail "Mic missing on both ALSA and PipeWire — Mode 2 will FALL BACK TO Mode 1"
fi

# ---------------------------------------------------------------------------
hdr "4. Default playback sink (headphones / speakers)"
# ---------------------------------------------------------------------------
if have pw-link; then
  PLAY="$(pw-link -i 2>/dev/null | grep -i playback | grep -vi hdmi || true)"
  if [[ -n "$PLAY" ]]; then
    pass "playback input ports (instrument links vocoder/dry/SL here):"
    echo "$PLAY" | head -n 12 | while read -r line; do info "$line"; done
  else
    fail "no non-HDMI playback_* ports found"
    fix  "Plug headphones/speakers; check Settings → Sound or: wpctl status"
  fi
fi
if have wpctl; then
  info "wpctl default sink:"
  wpctl status 2>/dev/null | sed -n '/Sinks:/,/Sources:/p' | head -n 20 | while read -r line; do info "$line"; done
  SINK_ID="$(wpctl status 2>/dev/null | grep -A20 'Sinks:' | grep '\*' | grep -oE '[0-9]+' | head -n1 || true)"
  if [[ -n "${SINK_ID:-}" ]]; then
    VOL="$(wpctl get-volume "$SINK_ID" 2>/dev/null || true)"
    info "default sink volume: $VOL"
    if echo "$VOL" | grep -qi muted; then
      fail "default sink is MUTED"
      fix  "wpctl set-mute $SINK_ID 0"
    else
      pass "default sink not muted"
    fi
  fi
fi

# ---------------------------------------------------------------------------
hdr "5. Smoke-test: launch Calf Vocoder host (temporary, ~3s)"
# ---------------------------------------------------------------------------
# Mirrors AudioGraphManager::launchVocoder()
JALV_BIN=""
for c in jalv jalv.gtk jalv.gtk3; do
  if have "$c"; then JALV_BIN="$c"; break; fi
done

VOC_TEST_OK=0
if [[ -z "$JALV_BIN" ]]; then
  fail "cannot smoke-test: no jalv binary"
elif [[ -z "$URI_FOUND" && -z "$VOCODER_URI" ]]; then
  fail "cannot smoke-test: no Vocoder URI"
else
  USE_URI="${VOCODER_URI}"
  info "starting: ${PWJACK:+pw-jack }$JALV_BIN -n $NODE_NAME $USE_URI"
  # Prefer pw-jack like the instrument
  if have pw-jack; then
    pw-jack "$JALV_BIN" -n "$NODE_NAME" "$USE_URI" >/tmp/diag_vocoder_launch.log 2>&1 &
  else
    "$JALV_BIN" -n "$NODE_NAME" "$USE_URI" >/tmp/diag_vocoder_launch.log 2>&1 &
  fi
  JP=$!
  # Wait for ports (up to ~4s)
  for _ in 1 2 3 4 5 6 7 8; do
    sleep 0.5
    VIN="$(pw-link -i 2>/dev/null | grep -iE "$NODE_NAME|Vocoder|calf" || true)"
    VOUT="$(pw-link -o 2>/dev/null | grep -iE "$NODE_NAME|Vocoder|calf" || true)"
    if [[ -n "$VIN" || -n "$VOUT" ]]; then
      break
    fi
  done
  if [[ -n "${VIN:-}" ]]; then
    pass "vocoder INPUT ports appeared:"
    echo "$VIN" | while read -r line; do info "$line"; done
    VOC_TEST_OK=1
  else
    fail "no vocoder INPUT ports after launch"
  fi
  if [[ -n "${VOUT:-}" ]]; then
    pass "vocoder OUTPUT ports appeared:"
    echo "$VOUT" | while read -r line; do info "$line"; done
    VOC_TEST_OK=1
  else
    fail "no vocoder OUTPUT ports after launch"
  fi
  if [[ "$VOC_TEST_OK" -eq 0 ]]; then
    fail "jalv host did not expose ports — Mode 2/3 graph cannot link"
    fix  "Check /tmp/diag_vocoder_launch.log"
    if [[ -f /tmp/diag_vocoder_launch.log ]]; then
      tail -n 30 /tmp/diag_vocoder_launch.log | while read -r line; do info "$line"; done
    fi
    fix  "Confirm URI: lv2ls | grep -i vocoder"
    fix  "Try interactive: pw-jack jalv.gtk -n InstrumentVocoder $USE_URI"
  else
    pass "vocoder host smoke-test OK (instrument can discover these names)"
  fi
  kill "$JP" 2>/dev/null || true
  wait "$JP" 2>/dev/null || true
  # Extra cleanup if jalv ignored SIGTERM
  pkill -f "InstrumentVocoder" 2>/dev/null || true
  sleep 0.3
fi

# ---------------------------------------------------------------------------
hdr "6. Expected graph (what the instrument will try to build)"
# ---------------------------------------------------------------------------
cat <<'EOF'
  Mode 2 (VocoderOnly):
    zynaddsubfx:out_1/2  -->  InstrumentVocoder inputs  (carrier)
    MVX2U capture        -->  InstrumentVocoder input   (modulator)
    InstrumentVocoder out -->  playback_FL / playback_FR
    (+ optional Zyn --> SooperLooper --> playback side chain)

  Mode 3 (SynthAndVocoder):
    everything in Mode 2, plus:
    MVX2U capture        -->  playback_FL / playback_FR  (dry parallel)

  If mic is absent at mode-switch time, Mode 2 FALLS BACK to Mode 1 (synth only).
  If jalv/ports fail, vocoder links are skipped; you may still hear dry/synth/SL only.
EOF

# ---------------------------------------------------------------------------
hdr "7. Summary"
# ---------------------------------------------------------------------------
if [[ "$FAILS" -eq 0 ]]; then
  pass "Preflight clean ($WARNS warning(s)). Safe to start the instrument and test Mode 2/3."
  info "Next: start the instrument, switch to Mode 2, then run: ./diag_mode23_live.sh"
else
  fail "$FAILS check(s) failed, $WARNS warning(s)."
  info "Fix the FAIL items above before expecting Mode 2/3 audio."
  info "Most common fixes:"
  fix  "sudo apt install -y calf-plugins jalv pipewire-jack liblo-tools alsa-utils"
  fix  "Plug MVX2U; verify: arecord -l | grep MVX2U"
  fix  "Unmute sink: wpctl set-mute @DEFAULT_AUDIO_SINK@ 0"
fi
exit "$FAILS"
