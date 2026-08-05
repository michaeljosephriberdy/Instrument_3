#!/usr/bin/env bash
#
# patch_layout_v1_4loop.sh
#
# Behavioral changes (no drive-by refactors):
#
#   1. SooperLooper launches with -l 4 (four independent loops). Per-loop
#      Record/Mute/Clear address /sl/<track>/hit (track 0..3). Overdub and
#      Undo act on a shared "armed" loop index (default 0; Record on a
#      track arms it). Mute All hits SooperLooper's broadcast loop index
#      (/sl/-1/hit) -- it mutes loops only, never the live Zyn path.
#   2. Physical Layout V1 note rails (absolute cents from the ET root row):
#        BottomLeft  rows 4..0 rails: 0, +50, +40, +30, +20 -- (14-c)*100+rail
#        BottomRight rows 4..0 rails: 0, -10, -20, -30, -40 -- c*100+rail
#        TopLeft  row 0 only: +10 rail, same horizontal rule as BottomLeft
#        TopRight row 0 only: -50 rail, same horizontal rule as BottomRight
#   3. Top boards reorganized (mirrored except row 0 notes):
#        Row 1: 15 percussion pads (existing GM map, unchanged)
#        Row 2: loop controls -- cols 0-2 Loop0 Rec/Mute/Clear, 3-5 Loop1,
#               6-8 Loop2, 9-11 Loop3, 12 Overdub(armed), 13 MuteAll,
#               14 Undo(armed)
#        Row 3: TransposeUp/Down, OctaveUp/Down, Panic, Mode1/2/3
#        Row 4: MasterVol+/-, VocoderMix+/- (frac of voice), DrumMix+/-
#               (frac of master), DryMix+/-  -- unchanged semantics
#   4. Volume semantics untouched: breath scales synth only against the
#      master ceiling; drum velocity = drumMix x master; dry/vocoder gains
#      as already designed. Bottom-board key count / ID75 geometry unchanged.
#
# Touches:
#   include/actions.h              (+loop_track field, +LoopMute/LoopMuteAll)
#   include/audio_graph_manager.h  (+per-loop/armed-loop API, +armed_loop_)
#   src/audio_graph_manager.cpp    (-l 4, per-loop OSC, mute-all, armed loop)
#   src/engine.cpp                 (loop action dispatch incl. loop_track)
#   src/layout_manager.cpp         (new rails, new top-board rows, JSON I/O)
#   config/layouts/*.json          (regenerated to match, if present)
#
# Run from the repo root (contains CMakeLists.txt, src/, include/).
# RESUMABLE: every change is guarded by a marker check and skipped (not
# re-applied, not a fatal error) if already present, so partial runs and
# re-runs are safe. .bak backups are taken once per file (never clobbered
# on a resume).

set -euo pipefail

CODE_FILES=(
  "include/actions.h"
  "include/audio_graph_manager.h"
  "src/audio_graph_manager.cpp"
  "src/engine.cpp"
  "src/layout_manager.cpp"
)

for f in "${CODE_FILES[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "ERROR: expected to find '$f' -- run this script from the repo root." >&2
    exit 1
  fi
done

echo "Backing up files to *.bak (skipping any that already exist) ..."
for f in "${CODE_FILES[@]}"; do
  if [[ -f "$f.bak" ]]; then
    echo "  $f.bak already exists -- leaving it alone (not overwriting)."
  else
    cp "$f" "$f.bak"
    echo "  backed up $f -> $f.bak"
  fi
done

python3 - "$@" <<'PYEOF'
import re

def ws(*lines):
    """Whitespace-tolerant regex: every line is split into tokens and all
    tokens (across all lines) are joined with \\s*, so neither indentation
    nor internal spacing has to match exactly -- only token order/content."""
    tokens = []
    for line in lines:
        tokens.extend(line.split())
    return r"\s*".join(re.escape(t) for t in tokens)

def patch(path, pattern, replacement, marker, expected=1, flags=re.MULTILINE):
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    if marker in text:
        print(f"  skip {path}: '{marker[:60]}' already present -- already applied")
        return
    new_text, n = re.subn(pattern, replacement, text, count=expected, flags=flags)
    if n != expected:
        raise SystemExit(
            f"ERROR: expected {expected} match(es) for pattern in {path}, "
            f"found {n}, and marker '{marker[:60]}' is not present either. "
            f"The source has likely drifted from what this script expects "
            f"-- inspect {path} (original saved as {path}.bak) and edit by "
            f"hand, or adjust the pattern in this script."
        )
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(new_text)
    print(f"  patched {path} ({n} change{'s' if n != 1 else ''})")

print("Patching layout / looper behavior...")

# =================================================================
# include/actions.h
# =================================================================

patch(
    "include/actions.h",
    ws(
        "LoopUndo,",
        "LoopClear,",
        "//-----------------------",
        "// Effects",
        "//-----------------------",
    ),
    "LoopUndo,\n"
    " LoopClear,\n"
    " LoopMute, // per-loop mute (Physical Layout V1 loop row)\n"
    " LoopMuteAll, // mutes all loops only -- never live Zyn\n"
    " //-----------------------\n"
    " // Effects\n"
    " //-----------------------",
    marker="LoopMuteAll, // mutes all loops only",
)

patch(
    "include/actions.h",
    ws(
        "int drum_note = 0;",
        "// True on key press, false on key release.",
    ),
    "int drum_note = 0;\n"
    " // Loop track index (0..3) for per-loop actions (LoopRecord/LoopMute/\n"
    " // LoopClear). Unused (0) for the shared-armed actions (LoopOverdub/\n"
    " // LoopUndo) and for LoopMuteAll.\n"
    " int loop_track = 0;\n"
    " // True on key press, false on key release.",
    marker="Loop track index (0..3) for per-loop actions",
)

patch(
    "include/actions.h",
    ws('case ActionType::LoopClear: return "loop_clear";'),
    'case ActionType::LoopClear: return "loop_clear";\n'
    ' case ActionType::LoopMute: return "loop_mute";\n'
    ' case ActionType::LoopMuteAll: return "loop_mute_all";',
    marker='case ActionType::LoopMuteAll: return "loop_mute_all";',
)

patch(
    "include/actions.h",
    ws('if (s == "loop_clear") return ActionType::LoopClear;'),
    'if (s == "loop_clear") return ActionType::LoopClear;\n'
    ' if (s == "loop_mute") return ActionType::LoopMute;\n'
    ' if (s == "loop_mute_all") return ActionType::LoopMuteAll;',
    marker='if (s == "loop_mute_all") return ActionType::LoopMuteAll;',
)

# =================================================================
# include/audio_graph_manager.h
# =================================================================

patch(
    "include/audio_graph_manager.h",
    ws(
        "void looperRecord();",
        "void looperOverdub();",
        "void looperPlay();",
        "void looperStop();",
        "void looperUndo();",
        "void looperClear();",
    ),
    "void looperRecord();\n"
    " void looperOverdub();\n"
    " void looperPlay();\n"
    " void looperStop();\n"
    " void looperUndo();\n"
    " void looperClear();\n"
    " // --- Per-loop control (4 independent loops, track index 0..3) ---\n"
    " void looperRecordTrack(int track);\n"
    " void looperMuteTrack(int track);\n"
    " void looperClearTrack(int track);\n"
    " // Mutes all loops only -- never the live Zyn output.\n"
    " void looperMuteAll();",
    marker="void looperMuteAll();",
)

patch(
    "include/audio_graph_manager.h",
    ws("int last_drums_ = 100;"),
    "int last_drums_ = 100;\n"
    " // Shared target for Overdub/Undo; armed by Record on that loop track.\n"
    " int armed_loop_ = 0;",
    marker="Shared target for Overdub/Undo; armed by Record",
)

# =================================================================
# src/audio_graph_manager.cpp
# =================================================================

patch(
    "src/audio_graph_manager.cpp",
    ws("// Headless: -p OSC port, -l 1 one loop, -c 2 stereo channels."),
    "// Headless: -p OSC port, -l 4 four independent loops, -c 2 stereo channels.",
    marker="-l 4 four independent loops",
)

patch(
    "src/audio_graph_manager.cpp",
    ws(
        '"-p", port.c_str(),',
        '"-l", "1",',
        '"-c", "2",',
        "static_cast<char*>(nullptr));",
        "}",
        'execlp(bin.c_str(), bin.c_str(),',
        '"-p", port.c_str(),',
        '"-l", "1",',
        '"-c", "2",',
        "static_cast<char*>(nullptr));",
    ),
    '"-p", port.c_str(),\n'
    ' "-l", "4",\n'
    ' "-c", "2",\n'
    ' static_cast<char*>(nullptr));\n'
    ' }\n'
    ' execlp(bin.c_str(), bin.c_str(),\n'
    ' "-p", port.c_str(),\n'
    ' "-l", "4",\n'
    ' "-c", "2",\n'
    ' static_cast<char*>(nullptr));',
    marker='"-l", "4",',
    expected=1,
)

patch(
    "src/audio_graph_manager.cpp",
    ws(
        "// --- Looper OSC (Phase 4.3 stubs) -------------------------------------------",
        "// SooperLooper OSC path examples (will be used when SL is confirmed running):",
        "// /sl/0/hit record",
        "// /sl/0/hit overdub",
        "// /sl/0/hit pause (or trigger)",
        "// /sl/0/hit undo",
        "// /sl/0/hit undo_all (clear-ish)",
        "// Prefer `oscsend` if present; otherwise log.",
        "namespace",
        "{",
        "void tryOsc(int port, const std::string& path, const std::string& arg = {})",
        "{",
        'std::string oscsend = which("oscsend");',
        "if (oscsend.empty())",
        "{",
        'Logger::info("Looper OSC " + path +',
        '" (oscsend not installed — install liblo-tools)");',
        "return;",
        "}",
        "std::ostringstream cmd;",
        'cmd << oscsend << " localhost " << port << " " << path;',
        "if (!arg.empty())",
        'cmd << " " << arg;',
        'cmd << " 2>/dev/null";',
        "int rc = std::system(cmd.str().c_str());",
        "if (rc != 0)",
        'Logger::warning("oscsend failed for " + path);',
        "else",
        'Logger::info("Looper OSC sent: " + path +',
        '(arg.empty() ? "" : " " + arg));',
        "}",
        "}",
        "void AudioGraphManager::looperRecord()",
        "{",
        "if (!processAlive(sl_pid_))",
        "{",
        "if (!launchSooperLooper())",
        "{",
        'Logger::info("Looper Record (SooperLooper unavailable)");',
        "return;",
        "}",
        "connectLooperGraph();",
        "}",
        'sendLooperOsc("/sl/0/hit", "s", "record");',
        "}",
        "void AudioGraphManager::looperOverdub()",
        "{",
        "if (!processAlive(sl_pid_))",
        "{",
        'Logger::info("Looper Overdub (SooperLooper not running)");',
        "return;",
        "}",
        'sendLooperOsc("/sl/0/hit", "s", "overdub");',
        "}",
        "void AudioGraphManager::looperPlay()",
        "{",
        "if (!processAlive(sl_pid_))",
        "{",
        'Logger::info("Looper Play (SooperLooper not running)");',
        "return;",
        "}",
        "// 'trigger' starts playback from the beginning; 'pause' toggles.",
        'sendLooperOsc("/sl/0/hit", "s", "trigger");',
        "}",
        "void AudioGraphManager::looperStop()",
        "{",
        "if (!processAlive(sl_pid_))",
        "{",
        'Logger::info("Looper Stop (SooperLooper not running)");',
        "return;",
        "}",
        "// Pause if playing; mute as a hard silence fallback.",
        'sendLooperOsc("/sl/0/hit", "s", "pause");',
        "}",
        "void AudioGraphManager::looperUndo()",
        "{",
        "if (!processAlive(sl_pid_))",
        "{",
        'Logger::info("Looper Undo (SooperLooper not running)");',
        "return;",
        "}",
        'sendLooperOsc("/sl/0/hit", "s", "undo");',
        "}",
        "void AudioGraphManager::looperClear()",
        "{",
        "if (!processAlive(sl_pid_))",
        "{",
        'Logger::info("Looper Clear (SooperLooper not running)");',
        "return;",
        "}",
        "// undo_all clears the loop content on common setups.",
        'sendLooperOsc("/sl/0/hit", "s", "undo_all");',
        "}",
    ),
    "// --- Looper OSC ---------------------------------------------------------\n"
    "// SooperLooper runs with 4 independent loops (-l 4). Per-loop control\n"
    "// uses /sl/<track>/hit (track 0..3); the \"armed\" loop (default 0,\n"
    "// re-armed by Record on that track) is the target for the shared\n"
    "// Overdub/Undo controls. Mute All addresses SooperLooper's broadcast\n"
    "// loop index (-1) so it only silences the loops -- never live Zyn.\n"
    "// Prefer `oscsend` if present; otherwise log (see sendLooperOsc()).\n"
    "namespace\n"
    "{\n"
    " std::string loopHitPath(int track)\n"
    " {\n"
    "     return \"/sl/\" + std::to_string(track) + \"/hit\";\n"
    " }\n"
    "}\n"
    "void AudioGraphManager::looperRecordTrack(int track)\n"
    "{\n"
    "    if (track < 0 || track > 3)\n"
    "        return;\n"
    "    if (!processAlive(sl_pid_))\n"
    "    {\n"
    "        if (!launchSooperLooper())\n"
    "        {\n"
    "            Logger::info(\"Looper Record track \" + std::to_string(track) +\n"
    "                         \" (SooperLooper unavailable)\");\n"
    "            return;\n"
    "        }\n"
    "        connectLooperGraph();\n"
    "    }\n"
    "    armed_loop_ = track; // Record arms this track for shared Overdub/Undo.\n"
    "    sendLooperOsc(loopHitPath(track), \"s\", \"record\");\n"
    "}\n"
    "void AudioGraphManager::looperMuteTrack(int track)\n"
    "{\n"
    "    if (track < 0 || track > 3)\n"
    "        return;\n"
    "    if (!processAlive(sl_pid_))\n"
    "    {\n"
    "        Logger::info(\"Looper Mute track \" + std::to_string(track) +\n"
    "                     \" (SooperLooper not running)\");\n"
    "        return;\n"
    "    }\n"
    "    sendLooperOsc(loopHitPath(track), \"s\", \"mute\");\n"
    "}\n"
    "void AudioGraphManager::looperClearTrack(int track)\n"
    "{\n"
    "    if (track < 0 || track > 3)\n"
    "        return;\n"
    "    if (!processAlive(sl_pid_))\n"
    "    {\n"
    "        Logger::info(\"Looper Clear track \" + std::to_string(track) +\n"
    "                     \" (SooperLooper not running)\");\n"
    "        return;\n"
    "    }\n"
    "    // undo_all clears the loop content on common setups.\n"
    "    sendLooperOsc(loopHitPath(track), \"s\", \"undo_all\");\n"
    "}\n"
    "void AudioGraphManager::looperMuteAll()\n"
    "{\n"
    "    if (!processAlive(sl_pid_))\n"
    "    {\n"
    "        Logger::info(\"Looper Mute All (SooperLooper not running)\");\n"
    "        return;\n"
    "    }\n"
    "    // Loop index -1 is SooperLooper's broadcast address -- every loop,\n"
    "    // and only the loops (live Zyn output is a separate, unaffected\n"
    "    // graph path).\n"
    "    sendLooperOsc(\"/sl/-1/hit\", \"s\", \"mute\");\n"
    "}\n"
    "void AudioGraphManager::looperRecord() { looperRecordTrack(0); }\n"
    "void AudioGraphManager::looperClear() { looperClearTrack(0); }\n"
    "void AudioGraphManager::looperOverdub()\n"
    "{\n"
    "    if (!processAlive(sl_pid_))\n"
    "    {\n"
    "        Logger::info(\"Looper Overdub (SooperLooper not running)\");\n"
    "        return;\n"
    "    }\n"
    "    sendLooperOsc(loopHitPath(armed_loop_), \"s\", \"overdub\");\n"
    "}\n"
    "void AudioGraphManager::looperPlay()\n"
    "{\n"
    "    if (!processAlive(sl_pid_))\n"
    "    {\n"
    "        Logger::info(\"Looper Play (SooperLooper not running)\");\n"
    "        return;\n"
    "    }\n"
    "    // 'trigger' starts playback from the beginning; 'pause' toggles.\n"
    "    sendLooperOsc(loopHitPath(armed_loop_), \"s\", \"trigger\");\n"
    "}\n"
    "void AudioGraphManager::looperStop()\n"
    "{\n"
    "    if (!processAlive(sl_pid_))\n"
    "    {\n"
    "        Logger::info(\"Looper Stop (SooperLooper not running)\");\n"
    "        return;\n"
    "    }\n"
    "    // Pause if playing; mute as a hard silence fallback.\n"
    "    sendLooperOsc(loopHitPath(armed_loop_), \"s\", \"pause\");\n"
    "}\n"
    "void AudioGraphManager::looperUndo()\n"
    "{\n"
    "    if (!processAlive(sl_pid_))\n"
    "    {\n"
    "        Logger::info(\"Looper Undo (SooperLooper not running)\");\n"
    "        return;\n"
    "    }\n"
    "    sendLooperOsc(loopHitPath(armed_loop_), \"s\", \"undo\");\n"
    "}",
    marker="std::string loopHitPath(int track)",
)

patch(
    "src/audio_graph_manager.cpp",
    ws(
        "void AudioGraphManager::connectLooperGraph()",
        "{",
        "if (!processAlive(sl_pid_))",
        "return;",
        "// Discover SL ports. Names vary slightly by version / pw-jack.",
    ),
    "void AudioGraphManager::connectLooperGraph()\n"
    "{\n"
    "    if (!processAlive(sl_pid_))\n"
    "        return;\n"
    "    // With -l 4, SooperLooper still exposes one common stereo in/out bus\n"
    "    // that sums all 4 loops -- per-loop OSC control does not require\n"
    "    // per-loop PipeWire ports, so the link topology below is unchanged.\n"
    "    // Discover SL ports. Names vary slightly by version / pw-jack.",
    marker="sums all 4 loops",
)

# =================================================================
# src/engine.cpp -- loop action dispatch
# =================================================================

patch(
    "src/engine.cpp",
    ws(
        "case ActionType::LoopRecord:",
        "case ActionType::LoopPlay:",
        "case ActionType::LoopStop:",
        "case ActionType::LoopOverdub:",
        "case ActionType::LoopUndo:",
        "case ActionType::LoopClear:",
        "switch (action.type)",
        "{",
        "case ActionType::LoopRecord: if (audio_) audio_->looperRecord(); break;",
        "case ActionType::LoopPlay: if (audio_) audio_->looperPlay(); break;",
        "case ActionType::LoopStop: if (audio_) audio_->looperStop(); break;",
        "case ActionType::LoopOverdub: if (audio_) audio_->looperOverdub(); break;",
        "case ActionType::LoopUndo: if (audio_) audio_->looperUndo(); break;",
        "case ActionType::LoopClear: if (audio_) audio_->looperClear(); break;",
        "default: break;",
        "}",
        "return;",
    ),
    "case ActionType::LoopRecord:\n"
    "            case ActionType::LoopMute:\n"
    "            case ActionType::LoopClear:\n"
    "            case ActionType::LoopPlay:\n"
    "            case ActionType::LoopStop:\n"
    "            case ActionType::LoopOverdub:\n"
    "            case ActionType::LoopUndo:\n"
    "            case ActionType::LoopMuteAll:\n"
    "                switch (action.type)\n"
    "                {\n"
    "                case ActionType::LoopRecord: if (audio_) audio_->looperRecordTrack(action.loop_track); break;\n"
    "                case ActionType::LoopMute: if (audio_) audio_->looperMuteTrack(action.loop_track); break;\n"
    "                case ActionType::LoopClear: if (audio_) audio_->looperClearTrack(action.loop_track); break;\n"
    "                case ActionType::LoopPlay: if (audio_) audio_->looperPlay(); break;\n"
    "                case ActionType::LoopStop: if (audio_) audio_->looperStop(); break;\n"
    "                case ActionType::LoopOverdub: if (audio_) audio_->looperOverdub(); break;\n"
    "                case ActionType::LoopUndo: if (audio_) audio_->looperUndo(); break;\n"
    "                case ActionType::LoopMuteAll: if (audio_) audio_->looperMuteAll(); break;\n"
    "                default: break;\n"
    "                }\n"
    "                return;",
    marker="audio_->looperRecordTrack(action.loop_track)",
)

# =================================================================
# src/layout_manager.cpp
# =================================================================

# --- rail offsets ---
patch(
    "src/layout_manager.cpp",
    ws("const int left_rail[5] = {10, 20, 30, 40, 0};"),
    "const int left_rail[5] = {20, 30, 40, 50, 0};",
    marker="const int left_rail[5] = {20, 30, 40, 50, 0};",
)

patch(
    "src/layout_manager.cpp",
    ws("const int right_rail[5] = {-50, -40, -30, -20, 0};"),
    "const int right_rail[5] = {-40, -30, -20, -10, 0};",
    marker="const int right_rail[5] = {-40, -30, -20, -10, 0};",
)

# --- makeLoop() helper, alongside makeNote/makeDrum/make ---
patch(
    "src/layout_manager.cpp",
    ws(
        "Action make(ActionType t)",
        "{",
        "Action a;",
        "a.type = t;",
        "return a;",
        "}",
    ),
    "Action make(ActionType t)\n"
    " {\n"
    " Action a;\n"
    " a.type = t;\n"
    " return a;\n"
    " }\n"
    " Action makeLoop(ActionType t, int track)\n"
    " {\n"
    " Action a;\n"
    " a.type = t;\n"
    " a.loop_track = track;\n"
    " return a;\n"
    " }",
    marker="Action makeLoop(ActionType t, int track)",
)

# --- Top Left / Top Right rebuild (rails + reorganized rows) ---
patch(
    "src/layout_manager.cpp",
    ws(
        "// ----- Top Left (command surface) -----",
        "// Row mapping (Physical Layout V1 labels):",
        "// row 0 — Loop controls",
        "// row 1 — Mode 1/2/3 + extras",
        "// row 2 — Mixer (master / vocoder / percussion)",
        "// row 3 — Percussion pads",
        "// row 4 — System (panic, transpose, empty for now)",
        "tl.name = \"top_left\";",
        "tl.inverted = true; // top boards mounted upside-down",
        "for (auto& a : tl.actions)",
        "a = Action{};",
        "// Row 0: Loop",
        "{",
        "const ActionType loop_row[15] = {",
        "ActionType::LoopRecord, ActionType::LoopPlay, ActionType::LoopStop,",
        "ActionType::LoopOverdub, ActionType::LoopUndo, ActionType::LoopClear,",
        "ActionType::None, ActionType::None, ActionType::None,",
        "ActionType::None, ActionType::None, ActionType::None,",
        "ActionType::None, ActionType::None, ActionType::None",
        "};",
        "for (int c = 0; c < ID75_COLS; ++c)",
        "tl.actions[index(0, c)] = make(loop_row[c]);",
        "}",
        "// Row 1: Modes",
        "{",
        "const ActionType mode_row[15] = {",
        "ActionType::ModeInstrument, ActionType::ModeVocoder, ActionType::ModeVocoderDry,",
        "ActionType::None, ActionType::None, ActionType::None,",
        "ActionType::None, ActionType::None, ActionType::None,",
        "ActionType::None, ActionType::None, ActionType::None,",
        "ActionType::None, ActionType::None, ActionType::None",
        "};",
        "for (int c = 0; c < ID75_COLS; ++c)",
        "tl.actions[index(1, c)] = make(mode_row[c]);",
        "}",
        "// Row 2: Mixer",
        "{",
        "const ActionType mix_row[15] = {",
        "ActionType::MasterVolUp, ActionType::MasterVolDown,",
        "ActionType::VocoderMixUp, ActionType::VocoderMixDown,",
        "ActionType::DrumMixUp, ActionType::DrumMixDown,",
        "ActionType::DryMixUp, ActionType::DryMixDown,",
        "ActionType::None, ActionType::None, ActionType::None,",
        "ActionType::None, ActionType::None, ActionType::None, ActionType::None",
        "};",
        "for (int c = 0; c < ID75_COLS; ++c)",
        "tl.actions[index(2, c)] = make(mix_row[c]);",
        "}",
        "// Row 3: Percussion",
        "for (int c = 0; c < ID75_COLS; ++c)",
        "tl.actions[index(3, c)] = makeDrum(DRUM_NOTES[c]);",
        "// Row 4: System",
        "{",
        "const ActionType sys_row[15] = {",
        "ActionType::Panic,",
        "ActionType::TransposeUp, ActionType::TransposeDown,",
        "ActionType::OctaveUp, ActionType::OctaveDown,",
        "ActionType::None, ActionType::None, ActionType::None,",
        "ActionType::None, ActionType::None, ActionType::None,",
        "ActionType::None, ActionType::None, ActionType::None, ActionType::None",
        "};",
        "for (int c = 0; c < ID75_COLS; ++c)",
        "tl.actions[index(4, c)] = make(sys_row[c]);",
        "}",
        "// ----- Top Right (mirror of top left per sheet) -----",
        "tr = tl;",
        "tr.name = \"top_right\";",
        "tr.inverted = true;",
        "}",
    ),
    "// ----- Top Left (command surface + rail-0 notes) -----\n"
    " // Row mapping (Physical Layout V1 labels):\n"
    " // row 0 — Note rail, +10, same horizontal rule as BottomLeft\n"
    " // row 1 — Percussion (15 pads, GM-ish map)\n"
    " // row 2 — Loop controls (per-loop Rec/Mute/Clear + shared Overdub/MuteAll/Undo)\n"
    " // row 3 — Transpose / Octave / Panic / Mode 1-2-3\n"
    " // row 4 — Mixer (MasterVol / VocoderMix / DrumMix / DryMix)\n"
    " tl.name = \"top_left\";\n"
    " tl.inverted = true; // top boards mounted upside-down\n"
    " for (auto& a : tl.actions)\n"
    " a = Action{};\n"
    " // Row 0: Note rail, +10 (same horizontal rule as BottomLeft).\n"
    " for (int c = 0; c < ID75_COLS; ++c)\n"
    " {\n"
    " int cents = (ID75_COLS - 1 - c) * 100 + 10;\n"
    " tl.actions[index(0, c)] = makeNote(cents);\n"
    " }\n"
    " // Row 1: Percussion — all 15 pads.\n"
    " for (int c = 0; c < ID75_COLS; ++c)\n"
    " tl.actions[index(1, c)] = makeDrum(DRUM_NOTES[c]);\n"
    " // Row 2: Loop controls.\n"
    " // cols 0-2 = Loop0 Rec/Mute/Clear, 3-5 = Loop1, 6-8 = Loop2, 9-11 = Loop3,\n"
    " // col 12 = Overdub (armed loop), 13 = Mute All, 14 = Undo (armed loop).\n"
    " for (int track = 0; track < 4; ++track)\n"
    " {\n"
    " int base = track * 3;\n"
    " tl.actions[index(2, base + 0)] = makeLoop(ActionType::LoopRecord, track);\n"
    " tl.actions[index(2, base + 1)] = makeLoop(ActionType::LoopMute, track);\n"
    " tl.actions[index(2, base + 2)] = makeLoop(ActionType::LoopClear, track);\n"
    " }\n"
    " tl.actions[index(2, 12)] = make(ActionType::LoopOverdub);\n"
    " tl.actions[index(2, 13)] = make(ActionType::LoopMuteAll);\n"
    " tl.actions[index(2, 14)] = make(ActionType::LoopUndo);\n"
    " // Row 3: Transpose / Octave / Panic / Mode 1-2-3.\n"
    " {\n"
    " const ActionType sys_row[15] = {\n"
    " ActionType::TransposeUp, ActionType::TransposeDown,\n"
    " ActionType::OctaveUp, ActionType::OctaveDown,\n"
    " ActionType::Panic,\n"
    " ActionType::ModeInstrument, ActionType::ModeVocoder, ActionType::ModeVocoderDry,\n"
    " ActionType::None, ActionType::None, ActionType::None,\n"
    " ActionType::None, ActionType::None, ActionType::None, ActionType::None\n"
    " };\n"
    " for (int c = 0; c < ID75_COLS; ++c)\n"
    " tl.actions[index(3, c)] = make(sys_row[c]);\n"
    " }\n"
    " // Row 4: Mixer.\n"
    " {\n"
    " const ActionType mix_row[15] = {\n"
    " ActionType::MasterVolUp, ActionType::MasterVolDown,\n"
    " ActionType::VocoderMixUp, ActionType::VocoderMixDown,\n"
    " ActionType::DrumMixUp, ActionType::DrumMixDown,\n"
    " ActionType::DryMixUp, ActionType::DryMixDown,\n"
    " ActionType::None, ActionType::None, ActionType::None,\n"
    " ActionType::None, ActionType::None, ActionType::None, ActionType::None\n"
    " };\n"
    " for (int c = 0; c < ID75_COLS; ++c)\n"
    " tl.actions[index(4, c)] = make(mix_row[c]);\n"
    " }\n"
    " // ----- Top Right (mirror of Top Left, except row 0 notes) -----\n"
    " tr = tl;\n"
    " tr.name = \"top_right\";\n"
    " tr.inverted = true;\n"
    " // Row 0: Note rail, -50 (same horizontal rule as BottomRight).\n"
    " for (int c = 0; c < ID75_COLS; ++c)\n"
    " {\n"
    " int cents = c * 100 - 50;\n"
    " tr.actions[index(0, c)] = makeNote(cents);\n"
    " }\n"
    "}",
    marker="Row 0: Note rail, +10 (same horizontal rule as BottomLeft).",
)

# --- JSON I/O: persist loop_track so on-disk layouts round-trip it ---
patch(
    "src/layout_manager.cpp",
    ws('a.drum_note = k.value("drum_note", 0);'),
    'a.drum_note = k.value("drum_note", 0);\n'
    ' a.loop_track = k.value("loop_track", 0);',
    marker='a.loop_track = k.value("loop_track", 0);',
)

patch(
    "src/layout_manager.cpp",
    ws(
        "if (a.type == ActionType::Drum)",
        'k["drum_note"] = a.drum_note;',
        "if (a.semitone != 0)",
        'k["semitone"] = a.semitone;',
    ),
    "if (a.type == ActionType::Drum)\n"
    " k[\"drum_note\"] = a.drum_note;\n"
    " if (a.loop_track != 0)\n"
    " k[\"loop_track\"] = a.loop_track;\n"
    " if (a.semitone != 0)\n"
    " k[\"semitone\"] = a.semitone;",
    marker='k["loop_track"] = a.loop_track;',
)

print("Code patches done.")
PYEOF

# =================================================================
# config/layouts/*.json -- regenerate to match, if the directory
# exists (JSON on disk takes precedence over the built-in C++
# layout at load time, so it has to be kept in sync).
# =================================================================
LAYOUTS_DIR="config/layouts"
if [[ -d "$LAYOUTS_DIR" ]]; then
  echo "Regenerating $LAYOUTS_DIR/*.json ..."
  python3 - "$LAYOUTS_DIR" <<'PYEOF'
import json, sys, os

layouts_dir = sys.argv[1]
VERSION_TAG = "v1.1-4loop-rails"  # idempotency / provenance marker

DRUM_NOTES = [36, 38, 46, 42, 51, 49, 45, 47, 50, 36, 38, 46, 42, 51, 49]
ROWS, COLS = 5, 15

def note(cents):
    return {"action": "note", "cents": cents}

def drum(n):
    return {"action": "drum", "drum_note": n}

def act(name):
    return {"action": name}

def loop(name, track):
    d = {"action": name}
    if track != 0:
        d["loop_track"] = track
    return d

def build_bottom(cents_fn):
    keys = []
    for r in range(ROWS):
        for c in range(COLS):
            k = {"row": r, "col": c}
            k.update(cents_fn(r, c))
            keys.append(k)
    return keys

def build_top(row0_cents_fn):
    keys = []
    for c in range(COLS):
        k = {"row": 0, "col": c}
        k.update(row0_cents_fn(c))
        keys.append(k)
    for c in range(COLS):
        keys.append({"row": 1, "col": c, **drum(DRUM_NOTES[c])})
    for track in range(4):
        base = track * 3
        keys.append({"row": 2, "col": base + 0, **loop("loop_record", track)})
        keys.append({"row": 2, "col": base + 1, **loop("loop_mute", track)})
        keys.append({"row": 2, "col": base + 2, **loop("loop_clear", track)})
    keys.append({"row": 2, "col": 12, **act("loop_overdub")})
    keys.append({"row": 2, "col": 13, **act("loop_mute_all")})
    keys.append({"row": 2, "col": 14, **act("loop_undo")})
    row3 = ["transpose_up", "transpose_down", "octave_up", "octave_down",
            "panic", "mode_instrument", "mode_vocoder", "mode_vocoder_dry"]
    for c, a in enumerate(row3):
        keys.append({"row": 3, "col": c, **act(a)})
    row4 = ["master_vol_up", "master_vol_down", "vocoder_mix_up", "vocoder_mix_down",
            "drum_mix_up", "drum_mix_down", "dry_mix_up", "dry_mix_down"]
    for c, a in enumerate(row4):
        keys.append({"row": 4, "col": c, **act(a)})
    return keys

def left_rail(r):
    return {0: 20, 1: 30, 2: 40, 3: 50, 4: 0}[r]

def right_rail(r):
    return {0: -40, 1: -30, 2: -20, 3: -10, 4: 0}[r]

layouts = {
    "bottom_left.json": {
        "name": "bottom_left", "inverted": False,
        "keys": build_bottom(lambda r, c: note((COLS - 1 - c) * 100 + left_rail(r))),
    },
    "bottom_right.json": {
        "name": "bottom_right", "inverted": False,
        "keys": build_bottom(lambda r, c: note(c * 100 + right_rail(r))),
    },
    "top_left.json": {
        "name": "top_left", "inverted": True,
        "keys": build_top(lambda c: note((COLS - 1 - c) * 100 + 10)),
    },
    "top_right.json": {
        "name": "top_right", "inverted": True,
        "keys": build_top(lambda c: note(c * 100 - 50)),
    },
}

for fname, data in layouts.items():
    path = os.path.join(layouts_dir, fname)
    if os.path.exists(path):
        try:
            with open(path) as fh:
                existing = json.load(fh)
            if existing.get("layout_version") == VERSION_TAG:
                print(f"  skip {path}: already regenerated ({VERSION_TAG})")
                continue
        except (json.JSONDecodeError, OSError):
            pass
        bak = path + ".bak"
        if not os.path.exists(bak):
            with open(path) as fh:
                orig = fh.read()
            with open(bak, "w") as fh:
                fh.write(orig)
            print(f"  backed up {path} -> {bak}")
    out = {"name": data["name"], "inverted": data["inverted"],
           "layout_version": VERSION_TAG, "keys": data["keys"]}
    with open(path, "w") as fh:
        json.dump(out, fh, indent=2)
        fh.write("\n")
    print(f"  wrote {path} ({len(data['keys'])} keys)")
PYEOF
else
  echo "No $LAYOUTS_DIR directory -- skipping JSON regeneration (built-in Physical Layout V1 will be used)."
fi

echo
echo "All patches applied (or already present)."
echo "Diff summary:"
for f in "${CODE_FILES[@]}"; do
  echo "--- $f ---"
  diff -u "$f.bak" "$f" || true
  echo
done

echo "Next step: rebuild (e.g. cmake --build build)."
