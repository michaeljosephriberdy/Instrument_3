#!/usr/bin/env bash
# diag_mode23_live.sh
#
# Run WHILE the instrument is running, AFTER switching to Mode 2 or Mode 3.
# Reports processes, ports, links, log evidence, and a fix checklist for
# whatever is missing vs what AudioGraphManager expects.
#
# Usage:
#   ./diag_mode23_live.sh
#   ./diag_mode23_live.sh /path/to/Instrument_3
#   LOG=./instrument.log ./diag_mode23_live.sh

set -uo pipefail

MIC_HINT="${MIC_HINT:-MVX2U}"
NODE_NAME="${NODE_NAME:-InstrumentVocoder}"
PROJECT_DIR="${1:-}"
LOG="${LOG:-}"

pass() { printf '  [PASS] %s\n' "$*"; }
fail() { printf '  [FAIL] %s\n' "$*"; }
warn() { printf '  [WARN] %s\n' "$*"; }
info() { printf '         %s\n' "$*"; }
fix()  { printf '  [FIX ] %s\n' "$*"; }
hdr()  { printf '\n=== %s ===\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# Resolve instrument.log
if [[ -z "$LOG" ]]; then
  for cand in \
    "${PROJECT_DIR:+$PROJECT_DIR/instrument.log}" \
    "./instrument.log" \
    "$(pwd)/instrument.log" \
    "$HOME/instrument.log"; do
    [[ -n "$cand" && -f "$cand" ]] && LOG="$cand" && break
  done
fi

hdr "0. Context"
info "time=$(date -Iseconds)  user=$(id -un)"
info "MIC_HINT=$MIC_HINT  NODE_NAME=$NODE_NAME"
info "log=${LOG:-'(not found — pass path or set LOG=...)'}"

# ---------------------------------------------------------------------------
hdr "1. Processes the instrument should own"
# ---------------------------------------------------------------------------
check_proc() {
  local pat="$1" label="$2"
  local lines
  lines="$(pgrep -af "$pat" 2>/dev/null || true)"
  if [[ -n "$lines" ]]; then
    pass "$label running"
    echo "$lines" | while read -r line; do info "$line"; done
    return 0
  else
    fail "$label NOT running"
    return 1
  fi
}

ZYN_OK=0; SL_OK=0; VOC_OK=0
check_proc 'zynaddsubfx' 'ZynAddSubFX' && ZYN_OK=1 || fix "Instrument should launch Zyn on start; check log for 'Launched Zyn'"
check_proc 'sooperlooper' 'SooperLooper' && SL_OK=1 || warn "SooperLooper down — looping side-chain unavailable (Mode 2/3 still possible)"
if check_proc 'jalv|InstrumentVocoder' 'Vocoder host (jalv)'; then
  VOC_OK=1
else
  fail "Vocoder host not running — Mode 2/3 cannot process carrier/modulator"
  fix  "Switch to Mode 2/3 in the UI (should call launchVocoder)."
  fix  "If still down: jalv/calf missing — run diag_mode23_preflight.sh"
  fix  "Manual test: pw-jack jalv -n InstrumentVocoder http://calf.sourceforge.net/plugins/Vocoder"
fi

# ---------------------------------------------------------------------------
hdr "2. Port inventory (what PipeWire currently exposes)"
# ---------------------------------------------------------------------------
if ! have pw-link; then
  fail "pw-link missing — install pipewire-bin"
  exit 1
fi

PW_IN="$(pw-link -i 2>/dev/null || true)"
PW_OUT="$(pw-link -o 2>/dev/null || true)"
PW_LINKS="$(pw-link -l 2>/dev/null || true)"

section_ports() {
  local title="$1" pattern="$2" stream="$3"
  local hits
  if [[ "$stream" == in ]]; then
    hits="$(echo "$PW_IN" | grep -iE "$pattern" || true)"
  else
    hits="$(echo "$PW_OUT" | grep -iE "$pattern" || true)"
  fi
  if [[ -n "$hits" ]]; then
    pass "$title"
    echo "$hits" | while read -r line; do info "$line"; done
    return 0
  else
    fail "$title — none"
    return 1
  fi
}

section_ports "Zyn outputs" 'zyn' out || fix "Zyn not publishing audio ports (process down or wrong JACK/PW backend)"
section_ports "Zyn inputs (MIDI etc.)" 'zyn' in || true
section_ports "Vocoder inputs" "$NODE_NAME|Vocoder|calf" in || {
  fix "Start Mode 2/3 so launchVocoder runs, or host manually with -n $NODE_NAME"
}
section_ports "Vocoder outputs" "$NODE_NAME|Vocoder|calf" out || true
section_ports "Mic ($MIC_HINT) inputs" "$MIC_HINT|Shure" in || {
  fix "Plug MVX2U; Mode 2 falls back to Mode 1 without mic"
}
section_ports "SooperLooper ports" 'sooperlooper|sooper' in || true
section_ports "SooperLooper outputs" 'sooperlooper|sooper' out || true
section_ports "Playback sinks (non-HDMI)" 'playback' in || fix "No playback ports — check default sink / headphones"

# ---------------------------------------------------------------------------
hdr "3. Active links (graph truth)"
# ---------------------------------------------------------------------------
show_links() {
  local label="$1" pattern="$2"
  local hits
  hits="$(echo "$PW_LINKS" | grep -iE "$pattern" || true)"
  if [[ -n "$hits" ]]; then
    pass "$label"
    echo "$hits" | head -n 40 | while read -r line; do info "$line"; done
    return 0
  else
    fail "$label — no matching links"
    return 1
  fi
}

show_links "Links involving Zyn" 'zyn' || true
show_links "Links involving Vocoder / jalv / calf" "$NODE_NAME|Vocoder|calf|jalv" || {
  fail "No vocoder links — carrier/modulator/out not connected"
  fix  "Confirm Mode 2/3 was selected (log should say Graph: Mode 2 or Mode 3)"
  fix  "If vocoder ports exist but no links: name mismatch or pw-link failed (see log)"
}
show_links "Links involving mic ($MIC_HINT / Shure)" "$MIC_HINT|Shure" || {
  warn "Mic not linked — no modulator and no dry path"
}
show_links "Links involving SooperLooper" 'sooper' || true

# Structured expectation checks
hdr "4. Expected Mode 2 / 3 edges (checklist)"

has_link_pair() {
  # Rough: both substrings appear on same link line or adjacent graph listing
  local a="$1" b="$2"
  echo "$PW_LINKS" | grep -i "$a" | grep -iq "$b"
}

# Carrier: zyn -> vocoder
if has_link_pair 'zyn' 'vocoder' || has_link_pair 'zyn' 'InstrumentVocoder' || has_link_pair 'zyn' 'calf' || has_link_pair 'zyn' 'jalv'; then
  pass "Carrier path: Zyn appears linked toward vocoder host"
else
  fail "Carrier path missing (Zyn not linked into vocoder inputs)"
  fix  "Need: pw-link 'zynaddsubfx:out_1' '<vocoder-in-port>' (instrument does this in buildMode2)"
fi

# Modulator: mic -> vocoder
if has_link_pair "$MIC_HINT" 'vocoder' || has_link_pair 'Shure' 'vocoder' || \
   has_link_pair "$MIC_HINT" 'InstrumentVocoder' || has_link_pair 'Shure' 'InstrumentVocoder' || \
   has_link_pair "$MIC_HINT" 'calf' || has_link_pair 'Shure' 'jalv'; then
  pass "Modulator path: mic appears linked into vocoder host"
else
  fail "Modulator path missing (mic not linked into vocoder)"
  fix  "Need mic capture -> vocoder input; without this you get carrier-only or silence"
fi

# Vocoder -> playback
if has_link_pair 'vocoder' 'playback' || has_link_pair 'InstrumentVocoder' 'playback' || \
   has_link_pair 'calf' 'playback' || has_link_pair 'jalv' 'playback'; then
  pass "Vocoder output appears linked to playback"
else
  fail "Vocoder -> playback link missing"
  fix  "Need InstrumentVocoder outs -> playback_FL/FR (or default headphones sink ports)"
fi

# Dry path (Mode 3 only) — mic -> playback without requiring vocoder on same line
DRY=0
if echo "$PW_LINKS" | grep -iE "$MIC_HINT|Shure" | grep -iq 'playback'; then
  pass "Possible Mode 3 dry path: mic linked toward playback"
  DRY=1
else
  warn "No mic->playback link seen (OK for Mode 2; required for Mode 3 dry)"
  fix  "For Mode 3: instrument should pw-link mic capture -> playback after buildMode2"
fi

# ---------------------------------------------------------------------------
hdr "5. Sink volume / mute"
# ---------------------------------------------------------------------------
if have wpctl; then
  SINK_ID="$(wpctl status 2>/dev/null | grep -A20 'Sinks:' | grep '\*' | grep -oE '[0-9]+' | head -n1 || true)"
  if [[ -n "${SINK_ID:-}" ]]; then
    VOL="$(wpctl get-volume "$SINK_ID" 2>/dev/null || true)"
    info "default sink id=$SINK_ID  $VOL"
    if echo "$VOL" | grep -qi muted; then
      fail "Default sink is MUTED — you will hear nothing"
      fix  "wpctl set-mute $SINK_ID 0"
    else
      pass "Default sink not muted"
    fi
  else
    warn "Could not parse default sink id from wpctl status"
  fi
else
  warn "wpctl not installed — skip mute check"
fi

# ---------------------------------------------------------------------------
hdr "6. Instrument log evidence (Mode / mic / vocoder / pw-link)"
# ---------------------------------------------------------------------------
if [[ -n "$LOG" && -f "$LOG" ]]; then
  info "using log: $LOG"
  echo "----- last Mode / Vocoder / mic / pw-link lines -----"
  grep -iE 'Mode [123]|Vocoder|mic |MVX2U|jalv|buildMode|Graph:|pw-link|falling back|vocoder in=|vocoder out=|Audio graph:' \
    "$LOG" 2>/dev/null | tail -n 60 || true
  echo "----- tail (last 25 lines) -----"
  tail -n 25 "$LOG" || true

  if grep -q 'falling back to Mode 1' "$LOG" 2>/dev/null; then
    fail "Log shows Mode 2 fell back to Mode 1 (mic missing at switch time)"
    fix  "Ensure MVX2U is present before selecting Mode 2"
  fi
  if grep -qi 'jalv not found' "$LOG" 2>/dev/null; then
    fail "Log: jalv not found"
    fix  "sudo apt install -y jalv calf-plugins"
  fi
  if grep -qi 'Calf Vocoder LV2 URI not found' "$LOG" 2>/dev/null; then
    fail "Log: Calf Vocoder URI not found in lv2ls"
    fix  "sudo apt install -y calf-plugins && lv2ls | grep -i vocoder"
  fi
  if grep -qiE 'vocoder in=0|vocoder out=0' "$LOG" 2>/dev/null; then
    fail "Log: vocoder port discovery returned 0"
    fix  "Host may be up under a name not matching needles InstrumentVocoder|Vocoder|calf|jalv"
    fix  "Compare pw-link -i/-o names to log 'voc in:' / 'voc out:' lines"
  fi
  if grep -qi 'Graph: Mode 2' "$LOG" 2>/dev/null; then
    pass "Log records Graph: Mode 2 built at least once"
  fi
  if grep -qi 'Graph: Mode 3' "$LOG" 2>/dev/null; then
    pass "Log records Graph: Mode 3 built at least once"
  fi
  if grep -qi 'Audio graph: mode=' "$LOG" 2>/dev/null; then
    info "Latest statusSummary-style lines:"
    grep 'Audio graph: mode=' "$LOG" | tail -n 5 | while read -r line; do info "$line"; done
  fi
else
  warn "No instrument.log found — cannot correlate software decisions"
  fix  "Re-run from project dir, or: LOG=/path/to/instrument.log $0"
fi

# ---------------------------------------------------------------------------
hdr "7. Diagnosis matrix (what to fix)"
# ---------------------------------------------------------------------------
cat <<EOF
  Symptom                         Likely cause                         Fix
  ------------------------------  -----------------------------------  ----------------------------------
  Mode 2 acts like Mode 1         Mic not seen at switch time          Plug MVX2U; check arecord -l
  voc=down in log                 jalv/calf missing or launch fail     preflight script; apt install
  vocoder ports exist, no links   pw-link failed / wrong names         Compare log voc in/out to pw-link
  Links OK, no vocoded sound      Calf levels / mic gain / wrong in    Speak into mic; raise MVX2U gain
  Mode 3 no dry voice             mic->playback link missing           Confirm Mode 3; check section 4
  Everything linked, silence      sink muted or wrong device           wpctl set-mute ID 0; select sink
  File exists on pw-link          Edge already linked (often OK)       Harmless if audio flows
EOF

hdr "8. Quick manual link probes (optional)"
info "If ports exist but software failed to link, you can test one edge by hand:"
info "  pw-link -l | head   # see exact names first"
info "  pw-link 'zynaddsubfx:out_1' '<exact-vocoder-in-port>'"
info "  pw-link '<exact-mic-capture>' '<exact-vocoder-mod-port>'"
info "  pw-link '<exact-vocoder-out>' '<exact-playback_FL>'"
info "Then re-run this script to confirm the new edges appear."

hdr "9. Snapshot summary"
info "Zyn=$ZYN_OK  SL=$SL_OK  VocHost=$VOC_OK  DryLinkGuess=$DRY"
if [[ "$ZYN_OK" -eq 1 && "$VOC_OK" -eq 1 ]]; then
  pass "Core processes for Mode 2/3 are up — use sections 3–4 for link health"
else
  fail "Core process gap — fix process section before chasing audio routing"
fi
