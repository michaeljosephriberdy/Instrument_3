#!/usr/bin/env bash
# patch_force_unlink.sh
# Permanent fix: before every mode graph is built, FORCE-unlink all
# instrument ports from SL and headphones (and clear auto-links that
# PipeWire creates when Zyn starts). owned_links_ alone is not enough.
#
# Run from Instrument_3 repo root, then rebuild.
set -euo pipefail

AGM="src/audio_graph_manager.cpp"
if [[ ! -f "$AGM" ]]; then
  echo "ERROR: run from Instrument_3 root" >&2
  exit 1
fi

TS="$(date +%Y%m%d_%H%M%S)"
cp "$AGM" "${AGM}.bak.forceunlink.${TS}"
echo "backed up → ${AGM}.bak.forceunlink.${TS}"

python3 - "$AGM" <<'PY'
import sys, pathlib, re

path = pathlib.Path(sys.argv[1])
text = path.read_text()

# ---------------------------------------------------------------------------
# Insert a helper forceDisconnectInstrumentGraph() and call it at the start
# of every buildMode* (right after disconnectAllOwnedLinks, or in place of
# relying on it alone).
# ---------------------------------------------------------------------------

helper = r'''
// Force-remove every link this instrument cares about, including ones
// PipeWire auto-created when Zyn/SL/jalv started (those never landed in
// owned_links_, so disconnectAllOwnedLinks alone cannot clear them).
void AudioGraphManager::forceDisconnectInstrumentGraph()
{
    auto kill = [this](const std::string& src, const std::string& dst) {
        if (!src.empty() && !dst.empty())
            pwUnlink(src, dst);
    };

    // Resolve current port names (same helpers the builders use).
    std::string mel_l = resolveClientPort(cfg_.zyn_client_name, "out_1", false);
    std::string mel_r = resolveClientPort(cfg_.zyn_client_name, "out_2", false);
    std::string dr_l  = resolveClientPort(cfg_.zyn_drums_client_name, "out_1", false);
    std::string dr_r  = resolveClientPort(cfg_.zyn_drums_client_name, "out_2", false);
    if (mel_l.empty()) mel_l = "zynaddsubfx_zyn-melody:out_1";
    if (mel_r.empty()) mel_r = "zynaddsubfx_zyn-melody:out_2";
    if (dr_l.empty())  dr_l  = "zynaddsubfx_zyn-drums:out_1";
    if (dr_r.empty())  dr_r  = "zynaddsubfx_zyn-drums:out_2";

    std::string sl_in_l = "sooperlooper:common_in_1";
    std::string sl_in_r = "sooperlooper:common_in_2";
    std::string sl_out_l = "sooperlooper:common_out_1";
    std::string sl_out_r = "sooperlooper:common_out_2";
    {
        std::string a, b, c, d;
        if (resolveLooperPorts(a, b, c, d)) {
            sl_in_l = a; sl_in_r = b; sl_out_l = c; sl_out_r = d;
        }
    }

    std::string play_l = defaultPlaybackPort(true);
    std::string play_r = defaultPlaybackPort(false);
    if (play_l.empty())
        play_l = "alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FL";
    if (play_r.empty())
        play_r = "alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FR";

    const std::string vin_l  = "InstrumentVocoder:in_l";
    const std::string vin_r  = "InstrumentVocoder:in_r";
    const std::string vside  = "InstrumentVocoder:sidechain";
    const std::string vside2 = "InstrumentVocoder:sidechain2";
    const std::string vout_l = "InstrumentVocoder:out_l";
    const std::string vout_r = "InstrumentVocoder:out_r";

    // melody anywhere
    for (const auto& s : {mel_l, mel_r}) {
        kill(s, sl_in_l); kill(s, sl_in_r);
        kill(s, play_l);  kill(s, play_r);
        kill(s, vin_l);   kill(s, vin_r);
    }
    // drums anywhere
    for (const auto& s : {dr_l, dr_r}) {
        kill(s, sl_in_l); kill(s, sl_in_r);
        kill(s, play_l);  kill(s, play_r);
    }
    // vocoder outs anywhere
    for (const auto& s : {vout_l, vout_r}) {
        kill(s, sl_in_l); kill(s, sl_in_r);
        kill(s, play_l);  kill(s, play_r);
    }
    // mic anywhere (capture ports)
    MicInfo mic = queryMic();
    for (const auto& s : mic.capture_ports) {
        kill(s, vin_l); kill(s, vin_r);
        kill(s, vside); kill(s, vside2);
        kill(s, sl_in_l); kill(s, sl_in_r);
        kill(s, play_l);  kill(s, play_r);
    }
    // SL → headphones
    kill(sl_out_l, play_l);
    kill(sl_out_r, play_r);

    // Also clear owned_links_ tracking so we start clean.
    owned_links_.clear();
    Logger::info("forceDisconnectInstrumentGraph: cleared auto + owned links");
}

'''

# Declare in header if possible — but for a minimal patch, make it a file-local
# static isn't right; it's a member. Add declaration to the .h and definition
# before buildMode1, then call it from each buildMode.

# --- header ---
hdr = pathlib.Path("include/audio_graph_manager.h")
if not hdr.exists():
    print("ERROR: include/audio_graph_manager.h missing", file=sys.stderr)
    sys.exit(1)
h = hdr.read_text()
if "forceDisconnectInstrumentGraph" not in h:
    # insert private declaration near disconnectAllOwnedLinks
    h2, n = re.subn(
        r'(void disconnectAllOwnedLinks\(\);)',
        r'\1\n    void forceDisconnectInstrumentGraph();',
        h, count=1,
    )
    if n != 1:
        # try near other private helpers
        h2, n = re.subn(
            r'(bool buildMode3\(\);)',
            r'\1\n    void forceDisconnectInstrumentGraph();',
            h, count=1,
        )
    if n != 1:
        print("ERROR: could not add declaration to header", file=sys.stderr)
        sys.exit(1)
    hdr.write_text(h2)
    print("[ok] declared forceDisconnectInstrumentGraph in header")
else:
    print("[ok] header already has forceDisconnectInstrumentGraph")

# --- cpp: insert definition just before buildMode1 ---
if "forceDisconnectInstrumentGraph" not in text:
    m = re.search(r'bool AudioGraphManager::buildMode1\(\)', text)
    if not m:
        print("ERROR: buildMode1 not found", file=sys.stderr)
        sys.exit(1)
    text = text[:m.start()] + helper + "\n" + text[m.start():]
    print("[ok] inserted forceDisconnectInstrumentGraph definition")
else:
    print("[ok] forceDisconnectInstrumentGraph definition already present")

# --- cpp: call it at the start of each buildMode (after disconnectAllOwnedLinks) ---
def inject_call(mode_name):
    global text
    # Match: bool AudioGraphManager::buildModeN() { ... disconnectAllOwnedLinks();
    pat = re.compile(
        rf'(bool AudioGraphManager::{mode_name}\(\)\s*\{{[\s\S]*?disconnectAllOwnedLinks\(\);)',
        re.MULTILINE,
    )
    def repl(m):
        block = m.group(1)
        if "forceDisconnectInstrumentGraph" in block:
            return block  # already injected in this function's opening
        return block + "\n    forceDisconnectInstrumentGraph();"
    new, n = pat.subn(repl, text, count=1)
    if n != 1:
        # buildMode3 may not call disconnectAllOwnedLinks (it calls buildMode2)
        if mode_name == "buildMode3":
            print(f"[ok] {mode_name}: relies on buildMode2 teardown (no extra inject)")
            return True
        print(f"WARNING: could not inject into {mode_name} (matches={n})", file=sys.stderr)
        return False
    text = new
    print(f"[ok] {mode_name} calls forceDisconnectInstrumentGraph")
    return True

inject_call("buildMode1")
inject_call("buildMode2")
inject_call("buildMode3")

# --- also after launchZyn / launchZynDrums: kill auto-connect to headphones ---
# Inject into launchZyn after ports become visible
launch_pat = re.compile(
    r'(Logger::info\("Melody Zyn ports visible"\);)',
)
def after_melody(m):
    return m.group(1) + '''
    // PipeWire often auto-links new JACK clients to the default sink.
    // Kill that immediately so Mode graphs own the routing.
    {
        std::string play_l = defaultPlaybackPort(true);
        std::string play_r = defaultPlaybackPort(false);
        if (play_l.empty())
            play_l = "alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FL";
        if (play_r.empty())
            play_r = "alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FR";
        std::string ol = resolveClientPort(cfg_.zyn_client_name, "out_1", false);
        std::string or_ = resolveClientPort(cfg_.zyn_client_name, "out_2", false);
        if (ol.empty()) ol = "zynaddsubfx_zyn-melody:out_1";
        if (or_.empty()) or_ = "zynaddsubfx_zyn-melody:out_2";
        pwUnlink(ol, play_l); pwUnlink(or_, play_r);
    }'''

text2, n = launch_pat.subn(after_melody, text, count=1)
if n == 1:
    text = text2
    print("[ok] launchZyn clears auto-link to headphones")
else:
    print("WARNING: could not patch launchZyn auto-link clear", file=sys.stderr)

drums_pat = re.compile(
    r'(Logger::info\("Drums Zyn ports visible"\);)',
)
def after_drums(m):
    return m.group(1) + '''
    {
        std::string play_l = defaultPlaybackPort(true);
        std::string play_r = defaultPlaybackPort(false);
        if (play_l.empty())
            play_l = "alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FL";
        if (play_r.empty())
            play_r = "alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FR";
        std::string ol = resolveClientPort(cfg_.zyn_drums_client_name, "out_1", false);
        std::string or_ = resolveClientPort(cfg_.zyn_drums_client_name, "out_2", false);
        if (ol.empty()) ol = "zynaddsubfx_zyn-drums:out_1";
        if (or_.empty()) or_ = "zynaddsubfx_zyn-drums:out_2";
        pwUnlink(ol, play_l); pwUnlink(or_, play_r);
    }'''

text2, n = drums_pat.subn(after_drums, text, count=1)
if n == 1:
    text = text2
    print("[ok] launchZynDrums clears auto-link to headphones")
else:
    print("WARNING: could not patch launchZynDrums auto-link clear", file=sys.stderr)

path.write_text(text)
print(f"[ok] wrote {path}")
PY

echo ""
echo "== Done. Rebuild:  cmake --build build"
echo "== Restart instrument. Auto-links to headphones will be cleared on"
echo "== every Zyn launch and every mode switch."
