#!/usr/bin/env bash
# fix_breath_volume.sh  (v2 -- replaces the Grok draft)
# Run from the repo root as your NORMAL user:
#   ./fix_breath_volume.sh             # patch, then compile-check (auto-rollback on failure)
#   ./fix_breath_volume.sh --no-build  # patch only
#
# End state (Mode 1 / SynthOnly only):
#   * Note-ons already go out at velocity 127 in Mode 1 -- left untouched.
#   * Melodic Zyn stays at full CC7 (127); breath no longer sends CC7.
#   * A real PipeWire node ("zyn-volume-node", a pw-loopback child) is created once.
#     Mode 1 wiring:  melody Zyn -> zyn-volume-node -> { SooperLooper, headphones }
#     Breath drives ONLY that node's volume: 0..63 linear 0.0..1.0, 64..127 = 1.0
#     (still scaled by your existing breath-max keys; default ceiling = 100%).
#   * Every other mode keeps its exact existing routing; node is forced to 100% there.
#   * Drums, vocoder routing, hot-plug, layouts, logger output: untouched.
# Safe to re-run (detects its own marker). Every anchor must match exactly once or
# NOTHING is written.  Backups: .fix_breath_bak_<timestamp>/
set -euo pipefail
DO_BUILD=1
[[ "${1:-}" == "--no-build" ]] && DO_BUILD=0
die() { echo "ERROR: $*" >&2; exit 1; }
[[ $EUID -ne 0 ]] || die "Run as your normal user, not root/sudo."
command -v python3 >/dev/null || die "python3 is required."
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO"
FILES=(src/engine.cpp src/audio_graph_manager.cpp include/audio_graph_manager.h)
for f in "${FILES[@]}"; do [[ -f "$f" ]] || die "missing $f (run from the repo root)"; done

TS="$(date +%Y%m%d_%H%M%S)"
BAK=".fix_breath_bak_${TS}"

python3 - "$BAK" "${FILES[@]}" <<'PYEOF'
import re, sys, os, shutil
bak, files = sys.argv[1], sys.argv[2:]
MARK = "[breath_vol_node]"
src = {f: open(f, encoding="utf-8").read() for f in files}
if any(MARK in t for t in src.values()):
    print("Already applied (marker found) -- nothing to do."); sys.exit(0)

def sub1(path, pattern, repl, flags=0, what=""):
    """Replace exactly one whitespace-tolerant match, else abort with nothing written."""
    n = len(re.findall(pattern, src[path], flags))
    if n != 1:
        sys.exit(f"ABORT: anchor '{what}' matched {n}x in {path} (need exactly 1). Nothing was changed.")
    src[path] = re.sub(pattern, lambda m: repl(m) if callable(repl) else repl, src[path], count=1, flags=flags)

def before(path, pattern, text, what):   # insert text before the anchor
    sub1(path, pattern, lambda m: text + m.group(0), what=what)
def after(path, pattern, text, what):    # insert text after the anchor
    sub1(path, pattern, lambda m: m.group(0) + text, what=what)

H, C, E = "include/audio_graph_manager.h", "src/audio_graph_manager.cpp", "src/engine.cpp"

# ---------------------------------------------------------------- header
after(H, r"#include <atomic>\n", "#include <thread>\n", "#include <atomic>")
after(H, r"bool micPresent\(\) const;",
      "\n    // [breath_vol_node] Mode-1 breath volume: 0.0..1.0 on the zyn-volume-node sink.\n"
      "    void setBreathVolume(float linear);", "micPresent decl")
after(H, r"bool launchVocoder\(\);",
      "\n    // [breath_vol_node] dedicated PipeWire volume node (pw-loopback child)\n"
      "    bool launchVolumeNode();\n"
      "    void volumeWorkerLoop();\n"
      "    void stopVolumeWorker();\n"
      "    void resolveVolumeNodePorts(std::string& in_l, std::string& in_r,\n"
      "                                std::string& out_l, std::string& out_r) const;",
      "launchVocoder decl")
sub1(H, r"MidiMel,\s*MidiDru\b", "MidiMel, MidiDru,\n        MelVol, VolSl, VolHp", what="LinkRole enum tail")
after(H, r"std::string mvx_l, mvx_r;", "\n        std::string vol_in_l, vol_in_r, vol_out_l, vol_out_r;", "PortCache mvx")
after(H, r"std::atomic<bool>\s+started_\{false\};",
      "\n    // [breath_vol_node]\n"
      "    pid_t vol_pid_ = -1;\n"
      "    bool vol_unavailable_ = false;\n"
      "    std::thread vol_thread_;\n"
      "    std::atomic<bool> vol_stop_{false};\n"
      "    std::atomic<int> vol_target_pct_{100};\n"
      "    std::atomic<int> vol_applied_pct_{-1};", "started_ member")

# ---------------------------------------------------------------- audio_graph_manager.cpp
# Mode 1 ONLY gets the node roles; Mode 4 keeps sharing the old direct roles.
sub1(C, r"case PerformanceMode::SynthOnly:\s*case PerformanceMode::BreathOctave:",
     "case PerformanceMode::SynthOnly:\n"
     "        return {R::MelVol, R::VolSl, R::VolHp, R::DrumSl, R::DrumHp, R::SlHp,\n"
     "                R::MidiMel, R::MidiDru};\n"
     "    case PerformanceMode::BreathOctave:", what="desiredRoles SynthOnly/BreathOctave")
# resolveRole (cold-start path, effectively dead code): keep the switch exhaustive.
before(C, r"case LinkRole::MelSl:\s*add2\(mel_l, mel_r, sl_in_l, sl_in_r\);\s*break;",
       "case LinkRole::MelVol:\n        case LinkRole::VolSl:\n        case LinkRole::VolHp:\n            break;  // [breath_vol_node] handled by the cached path\n        ",
       "resolveRole MelSl")
# cached link builder
before(C, r"auto roles = desiredRoles\(mode\);",
       "const bool vol_ok = !c.vol_in_l.empty() && !c.vol_in_r.empty() &&\n"
       "                        !c.vol_out_l.empty() && !c.vol_out_r.empty();  // [breath_vol_node]\n    ",
       "auto roles")
before(C, r"case LinkRole::MelSl:\s*add2\(c\.mel_l, c\.mel_r, c\.sl_in_l, c\.sl_in_r\);\s*break;",
       "case LinkRole::MelVol:\n"
       "            if (vol_ok) add2(c.mel_l, c.mel_r, c.vol_in_l, c.vol_in_r);\n"
       "            break;\n"
       "        case LinkRole::VolSl:\n"
       "            if (vol_ok) add2(c.vol_out_l, c.vol_out_r, c.sl_in_l, c.sl_in_r);\n"
       "            break;\n"
       "        case LinkRole::VolHp:\n"
       "            if (vol_ok) add2(c.vol_out_l, c.vol_out_r, c.play_l, c.play_r);\n"
       "            break;\n        ",
       "cache MelSl")
# graceful fallback: node missing -> Mode 1 links Zyn straight through (sound > breath volume)
before(C, r"mode_link_cache_\[idx\]\s*=\s*std::move\(want\);",
       "if (mode == PerformanceMode::SynthOnly && !vol_ok) {\n"
       "        Logger::warning(\"zyn-volume-node ports not found -- Mode 1 routing Zyn directly (no breath volume)\");\n"
       "        add2(c.mel_l, c.mel_r, c.sl_in_l, c.sl_in_r);\n"
       "        add2(c.mel_l, c.mel_r, c.play_l, c.play_r);\n"
       "    }\n    ", "cache std::move")
before(C, r"//\s*Force a fresh resolution so the cache is accurate for this mode\.",
       "if (mode == PerformanceMode::SynthOnly && !processAlive(vol_pid_))\n"
       "        launchVolumeNode();  // [breath_vol_node]\n    ", "force fresh resolution")
before(C, r"c\.midi_mel\s*=\s*findEngineMidiPort\(\);",
       "resolveVolumeNodePorts(c.vol_in_l, c.vol_in_r, c.vol_out_l, c.vol_out_r);  // [breath_vol_node]\n    ",
       "refreshPortCache midi_mel")
after(C, r"launchVocoder\(\);\s*//\s*keep host alive; Mode 1 simply leaves it unlinked",
      "\n    launchVolumeNode();  // [breath_vol_node] created once, lives for the whole run",
      "startProcesses launchVocoder")
sub1(C, r"disconnectAllOwnedLinks\(\);\s*killPid\(voc_pid_,\s*\"vocoder\"\);",
     lambda m: "disconnectAllOwnedLinks();\n    stopVolumeWorker();  // [breath_vol_node]\n"
               "    killPid(vol_pid_, \"zyn-volume-node\");\n    killPid(voc_pid_, \"vocoder\");",
     what="stopProcesses disconnect+killPid voc")
before(C, r"if \(!processAlive\(voc_pid_\)\) \{\s*Logger::warning\(\"Health: Vocoder host dead",
       "if (!vol_unavailable_ && !processAlive(vol_pid_)) {  // [breath_vol_node]\n"
       "        static auto vol_last_try = std::chrono::steady_clock::time_point{};\n"
       "        auto nowv = std::chrono::steady_clock::now();\n"
       "        if (vol_last_try.time_since_epoch().count() == 0 ||\n"
       "            nowv - vol_last_try > std::chrono::seconds(10)) {\n"
       "            vol_last_try = nowv;\n"
       "            Logger::warning(\"Health: zyn-volume-node dead -- relaunching\");\n"
       "            if (launchVolumeNode()) {\n"
       "                invalidatePortCache();\n"
       "                need_rebuild = true;\n"
       "            }\n"
       "        }\n"
       "    }\n    ", "health vocoder check")
after(C, r"mode_\s*=\s*mode;\s*//\s*already set inside apply, but keep consistent",
      "\n    if (mode != PerformanceMode::SynthOnly)\n"
      "        setBreathVolume(1.0f);  // [breath_vol_node] node fixed at 100% outside Mode 1",
      "setMode mode_ = mode")

NEWFUNCS = r'''

// ===== [breath_vol_node] dedicated PipeWire volume node =====================
// A pw-loopback child exposes a real sink ("zyn-volume-node", input side) and
// a playback stream ("zyn-volume-out", output side, autoconnect off). In Mode 1
// the melodic Zyn feeds the sink and the stream feeds SooperLooper + phones.
// Breath moves ONLY the sink volume. Volume pushes happen on a small worker
// thread (coalescing to the newest value) so the 1 ms performance loop never
// blocks on a pactl fork.
bool AudioGraphManager::launchVolumeNode()
{
    if (processAlive(vol_pid_))
        return true;
    if (vol_unavailable_)
        return false;
    const std::string bin = which("pw-loopback");
    if (bin.empty()) {
        vol_unavailable_ = true;
        Logger::warning("pw-loopback not found -- no breath volume node; "
                        "Mode 1 will route Zyn directly (no breath volume)");
        return false;
    }
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        execlp(bin.c_str(), bin.c_str(),
               "-n", "zyn-volume", "-c", "2", "-m", "[ FL FR ]",
               "--capture-props=media.class=Audio/Sink node.name=zyn-volume-node "
               "node.description=Zyn-Volume priority.session=0",
               "--playback-props=node.name=zyn-volume-out node.description=Zyn-Volume-Out "
               "node.autoconnect=false node.dont-reconnect=true",
               static_cast<char*>(nullptr));
        _exit(127);
    }
    vol_pid_ = pid;
    vol_applied_pct_.store(-1);  // fresh node: worker re-applies the current target
    Logger::info("Launched breath volume node (pw-loopback) pid=" + std::to_string(pid));
    bool ready = false;
    for (int i = 0; i < 25 && !ready; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ready = !findPortsMatching({"zyn-volume-node:"}, true).empty() &&
                !findPortsMatching({"zyn-volume-out:"}, false).empty();
    }
    if (ready)
        Logger::info("Breath volume node ports visible (zyn-volume-node)");
    else
        Logger::warning("Breath volume node ports not visible yet");
    if (!vol_thread_.joinable()) {
        vol_stop_.store(false);
        vol_thread_ = std::thread([this] { volumeWorkerLoop(); });
    }
    return true;
}

void AudioGraphManager::volumeWorkerLoop()
{
    const std::string pactl = which("pactl");
    while (!vol_stop_.load()) {
        const int want = vol_target_pct_.load();
        if (!pactl.empty() && want != vol_applied_pct_.load()) {
            const std::string cmd = pactl + " set-sink-volume zyn-volume-node " +
                                    std::to_string(want) + "% >/dev/null 2>&1";
            if (std::system(cmd.c_str()) == 0)
                vol_applied_pct_.store(want);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
}

void AudioGraphManager::stopVolumeWorker()
{
    vol_stop_.store(true);
    if (vol_thread_.joinable())
        vol_thread_.join();
    vol_stop_.store(false);
}

void AudioGraphManager::setBreathVolume(float linear)
{
    if (linear < 0.f) linear = 0.f;
    if (linear > 1.f) linear = 1.f;
    vol_target_pct_.store(static_cast<int>(linear * 100.0f + 0.5f));
}

void AudioGraphManager::resolveVolumeNodePorts(std::string& in_l, std::string& in_r,
                                               std::string& out_l, std::string& out_r) const
{
    in_l.clear(); in_r.clear(); out_l.clear(); out_r.clear();
    auto lower = [](std::string s) {
        for (char& ch : s)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    auto ends = [](const std::string& s, const char* suf) {
        const size_t n = std::strlen(suf);
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    auto pick = [&](const std::vector<std::string>& ports, std::string& l, std::string& r) {
        for (const auto& p : ports) {
            const std::string low = lower(p);
            if (low.find("monitor") != std::string::npos)
                continue;
            if (l.empty() && (ends(low, "_fl") || ends(low, "_l") || ends(low, "_1")))
                l = p;
            else if (r.empty() && (ends(low, "_fr") || ends(low, "_r") || ends(low, "_2")))
                r = p;
        }
    };
    pick(findPortsMatching({"zyn-volume-node:"}, true), in_l, in_r);
    pick(findPortsMatching({"zyn-volume-out:"}, false), out_l, out_r);
}
'''
src[C] = src[C].rstrip("\n") + "\n" + NEWFUNCS

# ---------------------------------------------------------------- engine.cpp
# Replace ONLY the CC7 send statement inside the existing Mode-1 breath block.
sub1(E, r"for\s*\(int ch = 0; ch < 16; \+\+ch\)\s*midi_->sendControlChange\(ch, breath_volume_cc_, scaled_volume\);",
     "if (breath_vol_resync_) {  // [breath_vol_node] Zyn stays at full CC7 in Mode 1\n"
     "                    for (int ch = 0; ch < 16; ++ch)\n"
     "                        midi_->sendControlChange(ch, breath_volume_cc_, 127);\n"
     "                }\n"
     "                // Breath -> node volume only: 0..63 linear 0..1, 64..127 = 1 (x breath-max ceiling).\n"
     "                if (audio_)\n"
     "                    audio_->setBreathVolume((static_cast<float>(breath_cc_lin) / 63.0f) *\n"
     "                                            (static_cast<float>(breath_max_) / 127.0f));",
     what="Mode-1 CC7 send loop")

# ---------------------------------------------------------------- write (all-or-nothing)
os.makedirs(bak, exist_ok=True)
for f in files:
    os.makedirs(os.path.join(bak, os.path.dirname(f)), exist_ok=True)
    shutil.copy2(f, os.path.join(bak, f))
for f in files:
    open(f, "w", encoding="utf-8").write(src[f])
print("Patched:", ", ".join(files))
print("Backup :", bak)
PYEOF

[[ -d "$BAK" ]] || exit 0   # marker found -> nothing changed

restore() { for f in "${FILES[@]}"; do cp -f "$BAK/$f" "$f"; done; }
if [[ $DO_BUILD -eq 1 ]]; then
  echo "=== Compile check ==="
  mkdir -p build
  if ! ( cd build && { [[ -f CMakeCache.txt ]] || cmake .. >/dev/null; } && cmake --build . -j"$(nproc)" ) ; then
    echo "BUILD FAILED -- restoring originals from $BAK" >&2
    restore
    exit 1
  fi
  echo "Build OK."
fi
cat <<'MSG'

Done. To verify at runtime (Mode 1):
  pactl list short sinks | grep zyn-volume-node     # the sink exists
  pw-link -io | grep zyn-volume                     # zyn-melody -> node -> sooperlooper/headphones
  watch -n0.2 "pactl get-sink-volume zyn-volume-node"   # follows breath
Undo: copy the files back from the .fix_breath_bak_* directory.
MSG
