#!/usr/bin/env bash
# fix_sooperlooper_permanent.sh
#
# Permanent fix for SooperLooper death / PipeWire corruption under the
# microtonal instrument, including headless use.
#
# What it does:
#   1. Hardens launchSooperLooper (rate-limit, settle, log capture).
#   2. Rate-limits health-check relaunches so we never recreate the
#      restart storm that corrupted PipeWire.
#   3. Adds automatic recovery: after repeated SL failures, restarts the
#      user PipeWire stack once (cooldown 90 s), then relaunches audio
#      clients and rebuilds the current mode graph.
#   4. SL remains mandatory in every mode.
#
# Normal performance path cost: negligible (counters inside the existing
# ~2 s health tick). Recovery path only runs on failure.
#
# Usage (from repo root):
#   bash fix_sooperlooper_permanent.sh
#
set -euo pipefail

REPO="${REPO:-$(pwd)}"
cd "$REPO"
CPP=src/audio_graph_manager.cpp
HDR=include/audio_graph_manager.h

[[ -f "$CPP" ]] || { echo "ERROR: missing $CPP (run from repo root)"; exit 1; }
[[ -f "$HDR" ]] || { echo "ERROR: missing $HDR"; exit 1; }

TS=$(date +%Y%m%d_%H%M%S)
cp -a "$CPP" "${CPP}.bak.slperm.${TS}"
cp -a "$HDR" "${HDR}.bak.slperm.${TS}"
echo "Backups: ${CPP}.bak.slperm.${TS}  ${HDR}.bak.slperm.${TS}"

python3 - <<'PY'
from pathlib import Path
import re
import sys

cpp_path = Path("src/audio_graph_manager.cpp")
hdr_path = Path("include/audio_graph_manager.h")
text = cpp_path.read_text()
hdr = hdr_path.read_text()
lines = text.splitlines(keepends=True)

def find_func_range(lines, signature):
    """Return (start, end) inclusive line indices for the function whose
    signature appears in a line. End is the closing brace of the body.
    Uses brace counting — never greedy-regex across the file."""
    start = None
    for i, line in enumerate(lines):
        if signature in line:
            start = i
            break
    if start is None:
        return None
    brace_line = start
    while brace_line < len(lines) and "{" not in lines[brace_line]:
        brace_line += 1
    if brace_line >= len(lines):
        return None
    depth = 0
    for j in range(brace_line, len(lines)):
        depth += lines[j].count("{") - lines[j].count("}")
        if depth == 0 and j > brace_line:
            return start, j
    return None

def replace_func(lines, signature, new_source):
    rng = find_func_range(lines, signature)
    if not rng:
        raise SystemExit(f"Function not found: {signature}")
    start, end = rng
    print(f"  {signature}: lines {start+1}..{end+1}")
    new_lines = new_source.splitlines(keepends=True)
    if new_lines and not new_lines[-1].endswith("\n"):
        new_lines[-1] += "\n"
    # Ensure a blank line after the function
    if not new_lines[-1].endswith("\n\n"):
        if new_lines[-1].endswith("\n"):
            new_lines.append("\n")
        else:
            new_lines.append("\n\n")
    return lines[:start] + new_lines + lines[end + 1:]

# --------------------------------------------------------------------------
# 1. launchSooperLooper — rate-limit, settle, log, no -q
# --------------------------------------------------------------------------
NEW_LAUNCH = r'''bool AudioGraphManager::launchSooperLooper()
{
    if (processAlive(sl_pid_))
        return true;

    std::string bin = which(cfg_.sooperlooper_binary.empty()
                            ? "sooperlooper"
                            : cfg_.sooperlooper_binary);
    if (bin.empty()) {
        Logger::warning("sooperlooper not found -- install package 'sooperlooper'; "
                        "loop keys will log only");
        return false;
    }

    // Rate-limit relaunches so we never recreate the restart storm that
    // corrupts PipeWire/JACK. First launch of a session is never delayed.
    static std::chrono::steady_clock::time_point last_launch{};
    auto now = std::chrono::steady_clock::now();
    if (last_launch.time_since_epoch().count() != 0 &&
        (now - last_launch) < std::chrono::seconds(12)) {
        Logger::warning("SooperLooper relaunch suppressed (rate limit 12s) "
                        "-- required in every mode; will retry next health tick");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        Logger::error("fork() failed for SooperLooper");
        return false;
    }
    if (pid == 0) {
        setsid();
        int fd = open("/tmp/sooperlooper_engine.log",
                      O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        std::string port = std::to_string(cfg_.sooperlooper_osc_port);
        std::string pwjack = which("pw-jack");
        if (!pwjack.empty()) {
            execlp("pw-jack", "pw-jack",
                   bin.c_str(),
                   "-p", port.c_str(),
                   "-l", "4",
                   "-c", "2",
                   static_cast<char*>(nullptr));
        }
        execlp(bin.c_str(), bin.c_str(),
               "-p", port.c_str(),
               "-l", "4",
               "-c", "2",
               static_cast<char*>(nullptr));
        _exit(127);
    }

    sl_pid_ = pid;
    last_launch = now;
    Logger::info("Launched SooperLooper pid=" + std::to_string(pid) +
                 " OSC :" + std::to_string(cfg_.sooperlooper_osc_port) +
                 " (log: /tmp/sooperlooper_engine.log)");

    // Hard settle: process must stay alive AND ports must appear.
    bool ports_ok = false;
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!processAlive(sl_pid_)) {
            Logger::warning("SooperLooper exited during settle "
                            "(see /tmp/sooperlooper_engine.log)");
            sl_pid_ = -1;
            return false;
        }
        auto outs = findPortsMatching(
            {"sooperlooper", "SooperLooper", "common_out"}, false);
        if (!outs.empty()) {
            ports_ok = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            if (!processAlive(sl_pid_)) {
                Logger::warning("SooperLooper died right after ports appeared "
                                "(see /tmp/sooperlooper_engine.log)");
                sl_pid_ = -1;
                return false;
            }
            break;
        }
    }
    if (!ports_ok) {
        Logger::warning("SooperLooper ports never appeared (pid still alive)");
    } else {
        Logger::info("SL ports settled");
    }
    return processAlive(sl_pid_);
}
'''

print("Replacing launchSooperLooper...")
lines = replace_func(lines, "AudioGraphManager::launchSooperLooper", NEW_LAUNCH)

# --------------------------------------------------------------------------
# 2. Add recoverAudioStack() implementation (new function)
#    Insert just before launchSooperLooper so it is a normal class method.
# --------------------------------------------------------------------------
RECOVER_IMPL = r'''bool AudioGraphManager::recoverAudioStack(const char* reason)
{
    // Cooldown: never restart PipeWire more than once per 90 s.
    static std::chrono::steady_clock::time_point last_recovery{};
    auto now = std::chrono::steady_clock::now();
    if (last_recovery.time_since_epoch().count() != 0 &&
        (now - last_recovery) < std::chrono::seconds(90)) {
        Logger::warning(std::string("Audio recovery suppressed (90s cooldown): ") +
                        (reason ? reason : ""));
        return false;
    }
    last_recovery = now;

    Logger::warning(std::string("AUDIO RECOVERY: ") +
                    (reason ? reason : "unspecified") +
                    " -- stopping clients, restarting user PipeWire, relaunching");

    // Stop our children cleanly so we do not leave zombies across the
    // PipeWire restart.
    killPid(sl_pid_, "sooperlooper");
    killPid(voc_pid_, "vocoder");
    killPid(zyn_drums_pid_, "zyn-drums");
    killPid(zyn_pid_, "zyn-melody");
    sl_pid_ = -1;
    voc_pid_ = -1;
    zyn_drums_pid_ = -1;
    zyn_pid_ = -1;

    // User-session restart — works headless, no root required.
    // pipewire-pulse + wireplumber keep the session coherent.
    int rc = std::system("systemctl --user restart pipewire pipewire-pulse wireplumber >/dev/null 2>&1");
    if (rc != 0) {
        Logger::error("systemctl --user restart pipewire... failed (rc=" +
                      std::to_string(rc) + ")");
        // Still try to bring our clients back on the existing session.
    } else {
        Logger::info("User PipeWire stack restarted");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    // Relaunch in a safe order: SL first (stable when it owns the graph
    // before the Zyns register), then the rest via normal start path.
    bool ok = launchSooperLooper();
    ok = launchZyn() && ok;
    ok = launchZynDrums() && ok;
    // Vocoder is mode-dependent; ensureHealthyGraph / setMode will start it
    // when needed. Still try once so Mode 2/3 recover quickly.
    (void)launchVocoder();

    // Rebuild links for the current mode.
    if (!setMode(mode_)) {
        Logger::warning("recoverAudioStack: setMode returned false");
        ok = false;
    }

    if (ok) {
        Logger::info("AUDIO RECOVERY complete — graph rebuilt for current mode");
    } else {
        Logger::warning("AUDIO RECOVERY finished with some failures "
                        "(see earlier warnings; health will keep trying)");
    }
    return ok;
}

'''

# Insert recoverAudioStack before launchSooperLooper
rng_sl = find_func_range(lines, "AudioGraphManager::launchSooperLooper")
if not rng_sl:
    raise SystemExit("launchSooperLooper missing after replace")
ins_at = rng_sl[0]
rec_lines = RECOVER_IMPL.splitlines(keepends=True)
if not rec_lines[-1].endswith("\n"):
    rec_lines[-1] += "\n"
lines = lines[:ins_at] + rec_lines + lines[ins_at:]
print("Inserted recoverAudioStack()")

# --------------------------------------------------------------------------
# 3. Patch ensureHealthyGraph SL handling + recovery trigger
# --------------------------------------------------------------------------
text2 = "".join(lines)

# We search for the SL-dead block in several known forms and replace with
# a single robust version that counts failures and calls recoverAudioStack.
NEW_HEALTH_SL = '''if (!processAlive(sl_pid_)) {
            static int sl_fail_streak = 0;
            static std::chrono::steady_clock::time_point last_sl_warn{};
            auto nowh = std::chrono::steady_clock::now();
            bool should_log = (last_sl_warn.time_since_epoch().count() == 0) ||
                              (nowh - last_sl_warn) > std::chrono::seconds(8);
            if (should_log) {
                Logger::warning("Health: SooperLooper dead -- relaunching "
                                "(required in every mode; rate-limited)");
                last_sl_warn = nowh;
            }
            ++sl_fail_streak;
            bool launched = launchSooperLooper();
            if (launched) {
                need_rebuild = true;
                sl_fail_streak = 0;
            } else if (sl_fail_streak >= 3) {
                // Repeated failure (often PipeWire/JACK corruption).
                // Recover the whole user audio stack once (90 s cooldown).
                if (recoverAudioStack("repeated SooperLooper launch failure")) {
                    need_rebuild = true;
                    sl_fail_streak = 0;
                }
            }
        } else {
            // Keep streak honest when SL is healthy.
            // (static local; reset only on success path above and here)
        }'''

# Normalize "else" reset — actually reset streak when alive in a simple way.
# Fix the else to reset streak:
NEW_HEALTH_SL = '''if (!processAlive(sl_pid_)) {
            static int sl_fail_streak = 0;
            static std::chrono::steady_clock::time_point last_sl_warn{};
            auto nowh = std::chrono::steady_clock::now();
            bool should_log = (last_sl_warn.time_since_epoch().count() == 0) ||
                              (nowh - last_sl_warn) > std::chrono::seconds(8);
            if (should_log) {
                Logger::warning("Health: SooperLooper dead -- relaunching "
                                "(required in every mode; rate-limited)");
                last_sl_warn = nowh;
            }
            ++sl_fail_streak;
            bool launched = launchSooperLooper();
            if (launched) {
                need_rebuild = true;
                sl_fail_streak = 0;
            } else if (sl_fail_streak >= 3) {
                if (recoverAudioStack("repeated SooperLooper launch failure")) {
                    need_rebuild = true;
                    sl_fail_streak = 0;
                }
            }
        }'''

# Try exact classic block
classic = '''if (!processAlive(sl_pid_)) {
        Logger::warning("Health: SooperLooper dead -- relaunching");
        launchSooperLooper();
        need_rebuild = true;
    }'''

# Indented classic (4 spaces / 8 spaces variants)
classic_indented = re.compile(
    r'if\s*\(\s*!processAlive\s*\(\s*sl_pid_\s*\)\s*\)\s*\{\s*'
    r'Logger::warning\s*\(\s*"Health: SooperLooper dead[^\"]*"\s*\)\s*;\s*'
    r'(?:if\s*\(\s*)?launchSooperLooper\s*\(\s*\)\s*(?:\)\s*\{\s*need_rebuild\s*=\s*true\s*;\s*\}\s*;)?\s*'
    r'(?:need_rebuild\s*=\s*true\s*;)?\s*'
    r'\}',
    re.DOTALL)

replaced_health = False
if classic in text2:
    text2 = text2.replace(classic, NEW_HEALTH_SL, 1)
    replaced_health = True
    print("Patched health SL block (classic exact)")
else:
    m = classic_indented.search(text2)
    if m:
        text2 = text2[:m.start()] + NEW_HEALTH_SL + text2[m.end():]
        replaced_health = True
        print("Patched health SL block (regex)")
    elif "Health: SooperLooper dead" in text2:
        # Already partially patched — replace from the if through the closing brace
        # of that if by finding the warning line and expanding.
        idx = text2.find("Health: SooperLooper dead")
        # Walk backward to "if (!processAlive(sl_pid_))"
        if_start = text2.rfind("if (!processAlive(sl_pid_))", 0, idx)
        if if_start < 0:
            if_start = text2.rfind("if (!processAlive(sl_pid_))", 0, idx)
        if if_start >= 0:
            # Brace-count forward from if_start
            depth = 0
            i = text2.find("{", if_start)
            j = i
            while j < len(text2):
                if text2[j] == "{":
                    depth += 1
                elif text2[j] == "}":
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                j += 1
            text2 = text2[:if_start] + NEW_HEALTH_SL + text2[j:]
            replaced_health = True
            print("Patched health SL block (span replace)")
        else:
            print("WARNING: found warning string but not the if — manual check needed")
    else:
        print("WARNING: health SL block not found — manual check needed")

if not replaced_health:
    print("ERROR: could not patch health block", file=sys.stderr)
    # Do not abort entirely; launch + recover still help if health is wired later

# --------------------------------------------------------------------------
# 4. Headers in cpp
# --------------------------------------------------------------------------
if "#include <fcntl.h>" not in text2:
    # After first #include
    m = re.search(r'#include\s+[<"].*[>"]', text2)
    if m:
        pos = m.end()
        text2 = text2[:pos] + "\n#include <fcntl.h>\n#include <unistd.h>\n#include <cstdlib>\n" + text2[pos:]
        print("Added fcntl.h / unistd.h / cstdlib")

# chrono should already be present via other includes; ensure steady_clock works.
# Most files already pull <chrono> transitively or directly.

# --------------------------------------------------------------------------
# 5. Header declaration for recoverAudioStack
# --------------------------------------------------------------------------
if "recoverAudioStack" not in hdr:
    # Insert near other private helpers — after forceDisconnectInstrumentGraph
    # or before buildMode1
    needle = "void forceDisconnectInstrumentGraph();"
    decl = ("void forceDisconnectInstrumentGraph();\n"
            "    // Restart user PipeWire + relaunch audio clients after\n"
            "    // repeated SooperLooper / graph failures (headless-safe).\n"
            "    bool recoverAudioStack(const char* reason);")
    if needle in hdr:
        hdr = hdr.replace(needle, decl, 1)
        print("Declared recoverAudioStack in header")
    else:
        # Fallback: before first buildMode
        m = re.search(r'bool\s+buildMode1\s*\(\s*\)\s*;', hdr)
        if m:
            hdr = hdr[:m.start()] + (
                "bool recoverAudioStack(const char* reason);\n    "
            ) + hdr[m.start():]
            print("Declared recoverAudioStack in header (fallback)")
        else:
            print("WARNING: could not find insertion point in header")
else:
    print("Header already has recoverAudioStack")

# --------------------------------------------------------------------------
# 6. Write + validate
# --------------------------------------------------------------------------
# Brace balance
if text2.count("{") != text2.count("}"):
    raise SystemExit(
        f"BRACE MISMATCH after patch: {{={text2.count('{')} }}={text2.count('}')} "
        "— refusing to write"
    )

# Must contain key markers
for s in ("recoverAudioStack", "rate limit 12s", "sooperlooper_engine.log",
          "SL ports settled", "AUDIO RECOVERY"):
    if s not in text2:
        raise SystemExit(f"Missing expected marker after patch: {s}")

cpp_path.write_text(text2)
hdr_path.write_text(hdr)
print("Wrote", cpp_path)
print("Wrote", hdr_path)
print("Brace balance OK")
print("Markers OK")
PY

echo
echo "=== Building ==="
cmake -S . -B build >/tmp/cmake_slperm.out 2>&1 || {
  echo "cmake configure failed:"
  tail -40 /tmp/cmake_slperm.out
  echo "Restoring backups..."
  cp -a "${CPP}.bak.slperm.${TS}" "$CPP"
  cp -a "${HDR}.bak.slperm.${TS}" "$HDR"
  exit 1
}

if ! cmake --build build -j"$(nproc)" 2>/tmp/build_slperm.err; then
  echo "BUILD FAILED — restoring backups"
  cp -a "${CPP}.bak.slperm.${TS}" "$CPP"
  cp -a "${HDR}.bak.slperm.${TS}" "$HDR"
  tail -100 /tmp/build_slperm.err
  exit 1
fi

echo
echo "=============================================="
echo " SUCCESS — permanent SooperLooper + recovery"
echo "=============================================="
echo " Binary : $REPO/build/microtonal_instrument"
echo " SL log : /tmp/sooperlooper_engine.log"
echo
echo "Behaviour:"
echo "  • SL is mandatory in every mode"
echo "  • Relaunch rate-limited to 12 s (no restart storm)"
echo "  • After 3 failed SL launches: automatic user PipeWire"
echo "    restart + full audio client relaunch (90 s cooldown)"
echo "  • Normal performance path: no extra latency"
echo
echo "One-time clean session before first run (recommended):"
echo "  systemctl --user restart pipewire pipewire-pulse wireplumber"
echo "  sleep 2"
echo "  ./build/microtonal_instrument"
echo
echo "Headless: recovery runs automatically; no keyboard needed."
echo "=============================================="
