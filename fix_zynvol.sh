#!/usr/bin/env bash
# fix_zynvol.sh -- replaces the slow Mode-1 "pw-loopback + pactl" volume node with zynvol,
# a tiny MIDI-controlled volume tool (JACK/PipeWire audio in/out, ALSA MIDI in).
#
# Run from the repo root as your NORMAL user (not sudo):
#   ./fix_zynvol.sh               # install deps, patch, build (auto-rollback on failure)
#   ./fix_zynvol.sh --no-build    # patch only
#   ./fix_zynvol.sh --skip-deps   # don't touch apt
#
# What changes (Mode 1 / SynthOnly only):
#   * NEW tools/zynvol/zynvol.cpp, built by CMake next to microtonal_instrument.
#   * The engine sends the SAME breath level it computed before (0..63 linear, flat 64..127,
#     x breath-max) to zynvol as MIDI (14-bit CC7/CC39) on a new ALSA port "MIDI Volume".
#     Nothing is sent to either Zyn: note velocity stays 127, no new CC to Zyn.
#   * launchVolumeNode() starts zynvol instead of pw-loopback; no more pactl fork per change.
#   * Same node name/ports (zyn-volume-node:in_1/in_2/out_1/out_2) so every existing link role,
#     health check, cache and the "node missing -> route Zyn directly" fallback keep working.
#   * Curve: gain = x^3, which is what `pactl set-sink-volume N%` did, so it sounds the same.
#     To make it truly linear instead, set ZYNVOL_EXPONENT=1.0 when running this script.
# Safe to re-run (marker [zynvol]). Every anchor must match exactly once or NOTHING is written.
# Backups: .fix_zynvol_bak_<timestamp>/
set -euo pipefail
DO_BUILD=1; DO_DEPS=1
for a in "$@"; do
  case "$a" in
    --no-build) DO_BUILD=0 ;;
    --skip-deps) DO_DEPS=0 ;;
    *) echo "Unknown argument: $a" >&2; exit 1 ;;
  esac
done
EXPONENT="${ZYNVOL_EXPONENT:-3.0}"
die() { echo "ERROR: $*" >&2; exit 1; }
[[ $EUID -ne 0 ]] || die "Run as your normal user, not root/sudo."
command -v python3 >/dev/null || die "python3 is required."
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO"
FILES=(CMakeLists.txt build.sh src/engine.cpp src/midi_engine.cpp src/audio_graph_manager.cpp include/midi_engine.h)
for f in "${FILES[@]}"; do [[ -f "$f" ]] || die "missing $f (run from the repo root)"; done

# ---- 1. JACK headers (the only new dependency; apt, amd64 + arm64) -----------------------
if [[ $DO_DEPS -eq 1 ]] && ! pkg-config --exists jack 2>/dev/null; then
  echo "=== Installing JACK development headers (needed to build zynvol) ==="
  installed=0
  for p in libjack-jackd2-dev libjack-dev; do
    apt-cache show "$p" >/dev/null 2>&1 || continue
    # Safety: never let apt remove packages (e.g. pipewire-jack) to satisfy this.
    if apt-get -s install "$p" 2>&1 | grep -q '^Remv'; then
      echo "Skipping $p: apt would remove installed packages."; continue
    fi
    sudo apt-get update && sudo apt-get install -y "$p" && { installed=1; break; }
  done
  [[ $installed -eq 1 ]] || die "Could not install JACK headers safely. Nothing was changed."
fi
if [[ $DO_DEPS -eq 1 ]]; then
  pkg-config --exists jack 2>/dev/null || die "jack.pc still not found after install. Nothing was changed."
fi

# ---- 2. The tool itself ------------------------------------------------------------------
mkdir -p tools/zynvol
cat > tools/zynvol/zynvol.cpp <<'ZYNVOL_EOF'
// zynvol -- tiny MIDI-controlled stereo volume (a "MIDI VCA") for PipeWire/JACK.
//
//   audio in  (2ch) --[ gain ]--> audio out (2ch)        JACK client "zyn-volume-node"
//   gain is set by MIDI from the instrument engine       ALSA sequencer port "zynvol:in"
//
// MIDI protocol (channel 0): CC 7 = MSB, CC 39 = LSB of a 14-bit value (0..16383).
// x = value/16383 is the instrument's breath level (0..1). gain = x^exponent.
// Default exponent 3.0 matches `pactl set-sink-volume N%` (PipeWire-Pulse applies a
// cubic law), i.e. the same response the old pw-loopback volume node had.
// Until the first MIDI message arrives the tool is a transparent pass-through.
#include <alsa/asoundlib.h>
#include <jack/jack.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::atomic<bool> g_run{true};
static std::atomic<float> g_target{1.0f};
static float g_cur = 1.0f;       // audio-thread only
static float g_coef = 0.01f;     // ~4 ms one-pole smoothing (anti-zipper)
static jack_port_t *g_in[2], *g_out[2];

static void on_signal(int) { g_run = false; }
static void on_jack_shutdown(void*) { g_run = false; }

static int process(jack_nframes_t n, void*) {
    const float tgt = g_target.load(std::memory_order_relaxed);
    float* in[2]  = {(float*)jack_port_get_buffer(g_in[0], n),  (float*)jack_port_get_buffer(g_in[1], n)};
    float* out[2] = {(float*)jack_port_get_buffer(g_out[0], n), (float*)jack_port_get_buffer(g_out[1], n)};
    float g = g_cur;
    for (jack_nframes_t i = 0; i < n; ++i) {
        g += (tgt - g) * g_coef;
        if (std::fabs(tgt - g) < 1e-6f) g = tgt;   // exact 0 stays exactly 0
        out[0][i] = in[0][i] * g;
        out[1][i] = in[1][i] * g;
    }
    g_cur = g;
    return 0;
}

// Find "<client_name>" / "<port_name>" in the ALSA sequencer and subscribe to it.
static bool connect_to_engine(snd_seq_t* seq, int my_port, const char* cname, const char* pname) {
    snd_seq_client_info_t* ci; snd_seq_port_info_t* pi;
    snd_seq_client_info_alloca(&ci); snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        if (std::strcmp(snd_seq_client_info_get_name(ci), cname) != 0) continue;
        const int cl = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, cl);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            if (std::strcmp(snd_seq_port_info_get_name(pi), pname) != 0) continue;
            const int r = snd_seq_connect_from(seq, my_port, cl, snd_seq_port_info_get_port(pi));
            return r >= 0 || r == -EEXIST;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    std::string name = "zyn-volume-node", mclient = "Instrument_3", mport = "MIDI Volume";
    float expo = 3.0f;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--name")) name = argv[i + 1];
        else if (!std::strcmp(argv[i], "--exponent")) expo = (float)std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--midi-client")) mclient = argv[i + 1];
        else if (!std::strcmp(argv[i], "--midi-port")) mport = argv[i + 1];
    }
    prctl(PR_SET_PDEATHSIG, SIGTERM);            // die with the instrument
    std::signal(SIGTERM, on_signal); std::signal(SIGINT, on_signal);

    jack_status_t st;
    jack_client_t* jc = jack_client_open(name.c_str(), JackNoStartServer, &st);
    if (!jc) { std::fprintf(stderr, "zynvol: cannot open JACK/PipeWire client\n"); return 2; }
    g_in[0]  = jack_port_register(jc, "in_1",  JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput,  0);
    g_in[1]  = jack_port_register(jc, "in_2",  JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput,  0);
    g_out[0] = jack_port_register(jc, "out_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_out[1] = jack_port_register(jc, "out_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_coef = 1.0f - std::exp(-1.0f / (0.004f * (float)jack_get_sample_rate(jc)));
    jack_set_process_callback(jc, process, nullptr);
    jack_on_shutdown(jc, on_jack_shutdown, nullptr);
    if (jack_activate(jc)) { std::fprintf(stderr, "zynvol: activate failed\n"); return 2; }

    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
        std::fprintf(stderr, "zynvol: cannot open ALSA sequencer (staying pass-through)\n");
        while (g_run) sleep(1);
        jack_client_close(jc); return 0;
    }
    snd_seq_set_client_name(seq, "zynvol");
    const int port = snd_seq_create_simple_port(seq, "in",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    const int npfd = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd* pfd = (struct pollfd*)std::calloc((size_t)npfd, sizeof(struct pollfd));
    snd_seq_poll_descriptors(seq, pfd, (unsigned)npfd, POLLIN);

    int msb = 0, lsb = 0, since_scan = 1000;
    while (g_run) {
        if (++since_scan >= 4) {                       // ~every 2 s: (re)subscribe to the engine
            since_scan = 0;
            connect_to_engine(seq, port, mclient.c_str(), mport.c_str());
        }
        if (poll(pfd, (nfds_t)npfd, 500) <= 0) continue;
        snd_seq_event_t* ev = nullptr;
        while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
            if (ev->type == SND_SEQ_EVENT_CONTROLLER) {
                const unsigned p = ev->data.control.param;
                const int v = (int)(ev->data.control.value & 127);
                if (p == 39) { lsb = v; }
                else if (p == 7) { msb = v; } else continue;
                const float x = (float)((msb << 7) | lsb) / 16383.0f;
                g_target.store(std::pow(x, expo), std::memory_order_relaxed);
            }
        }
    }
    snd_seq_close(seq);
    jack_client_close(jc);
    std::free(pfd);
    return 0;
}
ZYNVOL_EOF

# ---- 3. Patch (all-or-nothing) -----------------------------------------------------------
TS="$(date +%Y%m%d_%H%M%S)"
BAK=".fix_zynvol_bak_${TS}"
python3 - "$BAK" "$EXPONENT" "${FILES[@]}" <<'PYEOF'
import re, sys, os, shutil
bak, expo, files = sys.argv[1], sys.argv[2], sys.argv[3:]
MARK = "[zynvol]"
src = {f: open(f, encoding="utf-8").read() for f in files}
if any(MARK in t for t in src.values()):
    print("Already applied (marker found) -- nothing to do."); sys.exit(0)
def sub1(path, pattern, repl, flags=0, what=""):
    n = len(re.findall(pattern, src[path], flags))
    if n != 1:
        sys.exit(f"ABORT: anchor '{what}' matched {n}x in {path} (need exactly 1). Nothing was changed.")
    src[path] = re.sub(pattern, lambda m: repl(m) if callable(repl) else repl, src[path], count=1, flags=flags)
def before(p, pat, text, what): sub1(p, pat, lambda m: text + m.group(0), what=what)
def after(p, pat, text, what):  sub1(p, pat, lambda m: m.group(0) + text, what=what)
CM, BS, E, MC, AC, MH = "CMakeLists.txt", "build.sh", "src/engine.cpp", "src/midi_engine.cpp", "src/audio_graph_manager.cpp", "include/midi_engine.h"

# ---- CMake: build zynvol when jack.pc is available; otherwise warn and skip (never fatal)
src[CM] = src[CM].rstrip("\n") + '''

# [zynvol] MIDI-controlled volume helper for Mode 1 breath volume
pkg_check_modules(JACK QUIET jack)
if(JACK_FOUND)
  add_executable(zynvol tools/zynvol/zynvol.cpp)
  target_include_directories(zynvol PRIVATE ${JACK_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS})
  target_link_directories(zynvol PRIVATE ${JACK_LIBRARY_DIRS} ${ALSA_LIBRARY_DIRS})
  target_link_libraries(zynvol PRIVATE ${JACK_LIBRARIES} ${ALSA_LIBRARIES})
  target_compile_options(zynvol PRIVATE -Wall -Wextra)
else()
  message(WARNING "[zynvol] jack.pc not found (apt install libjack-jackd2-dev): Mode 1 will have no breath volume")
endif()
'''

# ---- build.sh: make future builds (Pi included) install the header package
sub1(BS, r"liblo-tools\)", "liblo-tools libjack-jackd2-dev)", what="build.sh package list")

# ---- MidiEngine: third ALSA port "MIDI Volume" + 14-bit sender (Zyn never sees it)
after(MH, r"int outputPort\(\) const;",
      "\n    // [zynvol] breath level 0..1 -> zynvol (14-bit CC7/CC39 on port \"MIDI Volume\")\n"
      "    bool sendBreathVolume(float x);\n    void resendBreathVolume();", "midi_engine.h outputPort")
after(MH, r"int port_drums_;", "\n    int port_volume_ = -1;\n    int last_breath_code_ = 0;", "midi_engine.h port_drums_")
before(MC, r"Logger::info\(\s*\"MIDI ports: 'MIDI Output' \(melody\) and 'MIDI Drums' \(drums\)\"\s*\);",
       '    port_volume_ = snd_seq_create_simple_port(  // [zynvol]\n'
       '        seq_, "MIDI Volume",\n'
       '        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,\n'
       '        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);\n'
       '    if (port_volume_ < 0)\n'
       '        Logger::warning("Could not create volume MIDI port (no breath volume in Mode 1)");\n    ',
       "midi_engine.cpp port log")
src[MC] = src[MC].rstrip("\n") + '''

// [zynvol] breath level (0..1) as a 14-bit controller pair, LSB first, to zynvol only.
bool MidiEngine::sendBreathVolume(float x)
{
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    const int v = static_cast<int>(std::lround(x * 16383.0f));
    last_breath_code_ = v;
    if (!seq_ || port_volume_ < 0)
        return false;
    snd_seq_event_t lsb;
    snd_seq_ev_clear(&lsb);
    snd_seq_ev_set_controller(&lsb, 0, 39, v & 127);
    const bool ok1 = sendEventOnPort(seq_, port_volume_, lsb);
    snd_seq_event_t msb;
    snd_seq_ev_clear(&msb);
    snd_seq_ev_set_controller(&msb, 0, 7, (v >> 7) & 127);
    const bool ok2 = sendEventOnPort(seq_, port_volume_, msb);
    return ok1 && ok2;
}

void MidiEngine::resendBreathVolume()
{
    sendBreathVolume(static_cast<float>(last_breath_code_) / 16383.0f);
}
'''

# ---- Engine: same level as before, now to zynvol instead of the pactl worker
sub1(E, r"if\s*\(\s*audio_\s*\)\s*audio_->setBreathVolume\(\s*\(static_cast<float>\(breath_cc_lin\)\s*/\s*63\.0f\)\s*\*\s*\(static_cast<float>\(breath_max_\)\s*/\s*127\.0f\)\s*\);",
     "if (midi_)  // [zynvol]\n"
     "                        midi_->sendBreathVolume((static_cast<float>(breath_cc_lin) / 63.0f) *\n"
     "                                                (static_cast<float>(breath_max_) / 127.0f));",
     what="engine setBreathVolume call")
# refresh on the existing 2 s health tick (heals a relaunched/reconnected zynvol)
after(E, r"next_graph_check\s*=\s*now\s*\+\s*std::chrono::seconds\(2\);\s*audio_->ensureHealthyGraph\(\);",
      "\n            if (midi_) midi_->resendBreathVolume();  // [zynvol]", "engine health tick")

# ---- AudioGraphManager: swap the node implementation, keep every signature & port name
mk = re.search(r"//\s*=====\s*\[breath_vol_node\]\s*dedicated PipeWire volume node", src[AC])
if not mk or len(re.findall(r"//\s*=====\s*\[breath_vol_node\]\s*dedicated PipeWire volume node", src[AC])) != 1:
    sys.exit("ABORT: volume-node block marker not found exactly once in " + AC + ". Nothing was changed.")
tail = src[AC][mk.start():]
defs = set(re.findall(r"\bAudioGraphManager::(\w+)\s*\(", tail))
allowed = {"launchVolumeNode", "volumeWorkerLoop", "stopVolumeWorker", "setBreathVolume", "resolveVolumeNodePorts"}
if not {"launchVolumeNode", "resolveVolumeNodePorts"} <= defs or not defs <= allowed or "namespace" in tail:
    sys.exit("ABORT: code after the volume-node marker is not the expected block (found %s). Nothing was changed." % sorted(defs))
NEWBLOCK = r'''// ===== [zynvol] MIDI-controlled volume node ================================
// zynvol is a tiny JACK/PipeWire client ("zyn-volume-node", stereo in/out) whose gain is
// set by MIDI from MidiEngine's "MIDI Volume" port. Same node/port names as the old
// pw-loopback node, so all link roles and the direct-routing fallback are unchanged.
bool AudioGraphManager::launchVolumeNode()
{
    if (processAlive(vol_pid_))
        return true;
    if (vol_unavailable_)
        return false;
    std::string bin;
    {
        char self[4096];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = '\0';
            const std::string exe(self);
            const size_t slash = exe.rfind('/');
            if (slash != std::string::npos) {
                const std::string cand = exe.substr(0, slash + 1) + "zynvol";
                if (access(cand.c_str(), X_OK) == 0)
                    bin = cand;
            }
        }
    }
    if (bin.empty() && access("build/zynvol", X_OK) == 0)
        bin = "build/zynvol";
    if (bin.empty())
        bin = which("zynvol");
    if (bin.empty()) {
        vol_unavailable_ = true;
        Logger::warning("zynvol not found (was it built? needs libjack-jackd2-dev) -- "
                        "Mode 1 will route Zyn directly (no breath volume)");
        return false;
    }
    const std::string pwjack = which("pw-jack");
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        setenv("PIPEWIRE_PROPS", "{ node.autoconnect=false }", 1);
        if (!pwjack.empty())
            execlp("pw-jack", "pw-jack", bin.c_str(), "--name", "zyn-volume-node",
                   "--exponent", "@EXPO@", static_cast<char*>(nullptr));
        execlp(bin.c_str(), bin.c_str(), "--name", "zyn-volume-node",
               "--exponent", "@EXPO@", static_cast<char*>(nullptr));
        _exit(127);
    }
    vol_pid_ = pid;
    Logger::info("Launched zynvol (MIDI volume node) pid=" + std::to_string(pid));
    bool ready = false;
    for (int i = 0; i < 25 && !ready; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!processAlive(vol_pid_))
            break;
        ready = !findPortsMatching({"zyn-volume-node:"}, true).empty() &&
                !findPortsMatching({"zyn-volume-node:"}, false).empty();
    }
    if (ready)
        Logger::info("zynvol ports visible (zyn-volume-node)");
    else
        Logger::warning("zynvol ports not visible yet");
    return true;
}

// Kept only so the header stays unchanged: there is no worker thread any more.
void AudioGraphManager::volumeWorkerLoop() {}

void AudioGraphManager::stopVolumeWorker()
{
    if (vol_thread_.joinable())
        vol_thread_.join();
}

// Breath volume now travels as MIDI (Engine -> MidiEngine -> zynvol); nothing to do here.
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
    pick(findPortsMatching({"zyn-volume-node:"}, false), out_l, out_r);
}
'''.replace("@EXPO@", expo)
src[AC] = src[AC][:mk.start()] + NEWBLOCK

# ---- write (all-or-nothing) ----
os.makedirs(bak, exist_ok=True)
for f in files:
    os.makedirs(os.path.join(bak, os.path.dirname(f)) if os.path.dirname(f) else bak, exist_ok=True)
    shutil.copy2(f, os.path.join(bak, f))
for f in files:
    open(f, "w", encoding="utf-8").write(src[f])
print("Patched:", ", ".join(files))
print("Backup :", bak)
PYEOF
[[ -d "$BAK" ]] || exit 0   # marker found -> nothing changed
restore() { for f in "${FILES[@]}"; do cp -f "$BAK/$f" "$f"; done; }

# ---- 4. Build (incremental; build/ is NOT wiped) -----------------------------------------
if [[ $DO_BUILD -eq 1 ]]; then
  echo "=== Build ==="
  JOBS="$(nproc)"
  case "$(uname -m)" in
    aarch64|arm*) mem_gb="$(awk '/MemTotal/ {printf "%d", $2/1048576}' /proc/meminfo)"
                  [[ "$mem_gb" -lt 1 ]] && mem_gb=1; (( JOBS > mem_gb )) && JOBS="$mem_gb" ;;
  esac
  mkdir -p build
  if ! ( cd build && { [[ -f CMakeCache.txt ]] || cmake .. >/dev/null; } && cmake .. >/dev/null && cmake --build . -j"$JOBS" ); then
    echo "BUILD FAILED -- restoring originals from $BAK" >&2
    restore; exit 1
  fi
  if [[ ! -x build/zynvol ]]; then
    echo "zynvol was not built (jack headers missing?) -- restoring originals from $BAK" >&2
    restore; exit 1
  fi
  echo "Build OK: build/microtonal_instrument + build/zynvol"
fi
cat <<'MSG'

Done. Start the instrument, go to Mode 1, then check:
  pw-link -io | grep zyn-volume-node        # in_1/in_2 fed by zyn-melody; out_1/out_2 -> SooperLooper + headphones
  aconnect -l | grep -B1 -A2 zynvol          # zynvol connected FROM Instrument_3 "MIDI Volume"
  grep -i zynvol instrument.log              # "Launched zynvol" / "ports visible"
  pw-link -l | grep -A1 zyn-volume-node:out  # no stray link to the default sink
Undo: copy the files in .fix_zynvol_bak_<timestamp>/ back over the repo.
MSG
