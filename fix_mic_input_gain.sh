#!/usr/bin/env bash
#
# fix_mic_input_gain.sh
#
# Permanently patches AudioGraphManager so the dry-mix buttons control the
# USB mic's real capture (input) gain, applied at the interface, instead of
# a downstream sink-volume call that only ever matches playback sinks
# (never a mic, which is a capture *source*).
#
# What it does to your real source tree:
#   1. include/audio_graph_manager.h
#        - adds a `setSourceVolume(...)` declaration next to setNodeVolume.
#   2. src/audio_graph_manager.cpp
#        - adds a `setSourceVolume(...)` implementation (mirrors
#          setNodeVolume, but queries `pactl list short sources` instead
#          of `... sinks`, and calls `pactl set-source-volume`).
#        - replaces the mic block inside applyMixerLevels() so mic capture
#          gain = dry/127.0 directly (not multiplied by master/vocoder,
#          not routed through setNodeVolume).
#
# Safe to re-run: it detects if the patch is already applied and skips.
# Every file it touches is backed up first, using this project's own
# ".bak.<label>.<timestamp>" convention, so nothing is destroyed.
#
# Usage:
#   ./fix_mic_input_gain.sh [--root /path/to/firmware] [--dry-run]
#
# Run it from (or point --root at) the directory that contains
# include/audio_graph_manager.h and src/audio_graph_manager.cpp.
#
# Requires only bash + awk + sed + grep (no python, no external deps).

set -euo pipefail

ROOT="."
DRY_RUN=0
MARKER="setSourceVolume"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --root) ROOT="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

HEADER="$ROOT/include/audio_graph_manager.h"
CPP="$ROOT/src/audio_graph_manager.cpp"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_AWK="$SCRIPT_DIR/_fix_mic_engine.awk"

for f in "$HEADER" "$CPP"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: $f not found. Run this from your firmware root, or pass --root /path/to/firmware." >&2
        exit 1
    fi
done

# ---------------------------------------------------------------------------
# Self-contained awk helper: splices new text into a C-family source file at
# points found by the same brace-matching approach as above. Two modes:
#   insert_after     -- insert ins_file's contents right after the closing
#                        brace of the function matching start_re.
#   replace_ifblock  -- within the function matching start_re, find the
#                        brace-delimited block introduced by inner_re and
#                        replace the whole thing (condition + body) with
#                        repl_file's contents.
# ---------------------------------------------------------------------------
cat > "$ENGINE_AWK" << 'AWK_EOF'
BEGIN {
    text = ""
    while ((getline line) > 0) text = text line "\n"
    n = length(text)

    start = find_line_start(start_re)
    if (start == 0) { print "ERROR: signature not found" > "/dev/stderr"; exit 1 }

    i = start
    while (i <= n && substr(text, i, 1) != "{") i++
    if (i > n) { print "ERROR: no opening brace after signature" > "/dev/stderr"; exit 1 }

    fn_open = i
    fn_close = match_brace(fn_open)
    if (fn_close == 0) { print "ERROR: unbalanced braces in function" > "/dev/stderr"; exit 1 }

    if (mode == "insert_after") {
        ins = slurp_file(ins_file)
        out = substr(text, 1, fn_close) "\n\n" ins substr(text, fn_close + 1)
        printf "%s", out
        exit 0
    }

    if (mode == "replace_ifblock") {
        fn_text = substr(text, start, fn_close - start + 1)
        inner_start = find_line_start_in(fn_text, inner_re)
        if (inner_start == 0) { print "ERROR: inner block not found" > "/dev/stderr"; exit 1 }

        fnlen = length(fn_text)
        j = inner_start
        while (j <= fnlen && substr(fn_text, j, 1) != "{") j++
        if (j > fnlen) { print "ERROR: no opening brace for inner block" > "/dev/stderr"; exit 1 }

        inner_open = j
        inner_close = match_brace_in(fn_text, inner_open)
        if (inner_close == 0) { print "ERROR: unbalanced braces in inner block" > "/dev/stderr"; exit 1 }

        repl = slurp_file(repl_file)
        new_fn_text = substr(fn_text, 1, inner_start - 1) repl substr(fn_text, inner_close + 1)
        out = substr(text, 1, start - 1) new_fn_text substr(text, fn_close + 1)
        printf "%s", out
        exit 0
    }

    print "ERROR: unknown mode " mode > "/dev/stderr"
    exit 1
}

function find_line_start(re,    pos, nl, linelen, thisline, found) {
    found = 0
    pos = 1
    while (pos <= n) {
        nl = index(substr(text, pos), "\n")
        linelen = (nl == 0) ? (n - pos + 1) : nl - 1
        thisline = substr(text, pos, linelen)
        if (match(thisline, re)) { found = pos + RSTART - 1; break }
        pos += linelen + 1
    }
    return found
}

function find_line_start_in(s, re,    pos, nl, linelen, thisline, found, sn) {
    sn = length(s)
    found = 0
    pos = 1
    while (pos <= sn) {
        nl = index(substr(s, pos), "\n")
        linelen = (nl == 0) ? (sn - pos + 1) : nl - 1
        thisline = substr(s, pos, linelen)
        if (match(thisline, re)) { found = pos + RSTART - 1; break }
        pos += linelen + 1
    }
    return found
}

function match_brace(open,    depth, j, c, in_str) {
    depth = 0; j = open; in_str = 0
    while (j <= n) {
        c = substr(text, j, 1)
        if (in_str) {
            if (c == "\\") { j += 2; continue }
            if (c == "\"") in_str = 0
            j++; continue
        }
        if (c == "\"") { in_str = 1; j++; continue }
        if (c == "{") depth++
        else if (c == "}") { depth--; if (depth == 0) return j }
        j++
    }
    return 0
}

function match_brace_in(s, open,    depth, j, c, in_str, sn) {
    sn = length(s)
    depth = 0; j = open; in_str = 0
    while (j <= sn) {
        c = substr(s, j, 1)
        if (in_str) {
            if (c == "\\") { j += 2; continue }
            if (c == "\"") in_str = 0
            j++; continue
        }
        if (c == "\"") { in_str = 1; j++; continue }
        if (c == "{") depth++
        else if (c == "}") { depth--; if (depth == 0) return j }
        j++
    }
    return 0
}

function slurp_file(path,    s, l) {
    s = ""
    while ((getline l < path) > 0) s = s l "\n"
    close(path)
    return s
}
AWK_EOF

backup() {
    local file="$1" label="$2"
    local ts; ts="$(date +%Y%m%d_%H%M%S)"
    local bak="${file}.bak.${label}.${ts}"
    if [[ "$DRY_RUN" -eq 0 ]]; then
        cp "$file" "$bak"
    fi
    echo "  backup -> $bak"
}

# ---------------------------------------------------------------------------
# 1. Header: insert the setSourceVolume() declaration after setNodeVolume()'s.
# ---------------------------------------------------------------------------
patch_header() {
    if grep -q "$MARKER" "$HEADER"; then
        echo "[skip] $HEADER already patched."
        return 0
    fi

    if ! grep -Eq 'bool[[:space:]]+setNodeVolume[[:space:]]*\([[:space:]]*const[[:space:]]+std::string&[[:space:]]*node_name_substring[[:space:]]*,[[:space:]]*float[[:space:]]+linear[[:space:]]*\)[[:space:]]*;' "$HEADER"; then
        echo "ERROR: could not find the setNodeVolume(...) declaration in $HEADER." >&2
        echo "  The file may have been reformatted -- add the setSourceVolume declaration next to it by hand." >&2
        exit 1
    fi

    echo "[patch] $HEADER"
    backup "$HEADER" "micgainfix"

    awk '
        /bool[[:space:]]+setNodeVolume[[:space:]]*\(.*node_name_substring.*float[[:space:]]+linear[[:space:]]*\)[[:space:]]*;/ && !done {
            print
            print "    // Same idea, but for a capture SOURCE (mic/input), not a playback sink --"
            print "    // setNodeVolume() alone can never find a USB mic, since it only queries sinks."
            print "    bool setSourceVolume(const std::string& node_name_substring, float linear);"
            done = 1
            next
        }
        { print }
    ' "$HEADER" > "$HEADER.tmp"

    if [[ "$DRY_RUN" -eq 0 ]]; then
        mv "$HEADER.tmp" "$HEADER"
    else
        rm -f "$HEADER.tmp"
    fi
}

# ---------------------------------------------------------------------------
# 2. CPP: (a) insert setSourceVolume() impl after setNodeVolume()'s impl.
#         (b) replace the mic block inside applyMixerLevels().
# ---------------------------------------------------------------------------
patch_cpp() {
    if grep -q "$MARKER" "$CPP"; then
        echo "[skip] $CPP already patched."
        return 0
    fi

    echo "[patch] $CPP"
    backup "$CPP" "micgainfix"

    # --- (a) insert setSourceVolume()'s implementation after setNodeVolume()'s ---
    cat > "$CPP.newfn" << 'FN_EOF'
bool AudioGraphManager::setSourceVolume(const std::string& node_name_substring,
                                         float linear)
{
    // Mirrors setNodeVolume(), but for a capture SOURCE (mic/input)
    // instead of a playback sink. A USB mic shows up in PipeWire/ALSA as
    // a source, never a sink, so setNodeVolume() alone can never find it
    // -- that was the reason the dry-mix buttons never touched real mic
    // hardware gain.
    if (linear < 0.f) linear = 0.f;
    if (linear > 1.f) linear = 1.f;

    std::string pactl = which("pactl");
    if (!pactl.empty())
    {
        std::string sources = shellCapture(pactl + " list short sources 2>/dev/null");
        std::istringstream ss(sources);
        std::string line;
        while (std::getline(ss, line))
        {
            if (line.find(node_name_substring) == std::string::npos)
                continue;
            // Skip monitor sources ("...analog-stereo.monitor") -- those
            // mirror a sink's playback, not the mic's real capture input.
            if (line.find(".monitor") != std::string::npos)
                continue;
            std::istringstream ls(line);
            std::string id, name;
            ls >> id >> name;
            int percent = static_cast<int>(linear * 100.0f + 0.5f);
            std::string setcmd = pactl + " set-source-volume " + id + " " +
                                  std::to_string(percent) + "% 2>/dev/null";
            if (std::system(setcmd.c_str()) == 0)
            {
                Logger::info("Input gain " + name + " -> " + std::to_string(percent) + "%");
                return true;
            }
        }
    }
    Logger::debug("setSourceVolume '" + node_name_substring + "' linear=" +
                  std::to_string(linear) + " (no matching source; recorded only)");
    return false;
}
FN_EOF

    if ! awk -v mode="insert_after" \
              -v start_re='bool[[:space:]]+AudioGraphManager::setNodeVolume' \
              -v ins_file="$CPP.newfn" \
              -f "$ENGINE_AWK" "$CPP" > "$CPP.step1" 2>"$CPP.err"; then
        cat "$CPP.err" >&2
        rm -f "$CPP.newfn" "$CPP.step1" "$CPP.err"
        exit 1
    fi
    rm -f "$CPP.newfn" "$CPP.err"

    # --- (b) replace the mic block inside applyMixerLevels() ---
    cat > "$CPP.newblock" << 'BLOCK_EOF'
if (!cfg_.mic_name_hint.empty())
    {
        // The dry-mix buttons ARE the mic's input-gain buttons: apply the
        // level directly at the USB interface's capture source, as a flat
        // fraction of whatever the mic picks up. Deliberately NOT
        // multiplied by master/vocoder -- those are downstream mix
        // decisions; this is the input stage, before anything else
        // touches the signal. dry=64 (50%) means capture gain = 50%,
        // full stop.
        float mic_capture_gain = static_cast<float>(dry) / 127.0f;
        setSourceVolume(cfg_.mic_name_hint, mic_capture_gain);
    }
BLOCK_EOF

    if ! awk -v mode="replace_ifblock" \
              -v start_re='void[[:space:]]+AudioGraphManager::applyMixerLevels' \
              -v inner_re='if[[:space:]]*\\([[:space:]]*![[:space:]]*cfg_\\.mic_name_hint\\.empty[[:space:]]*\\([[:space:]]*\\)[[:space:]]*\\)' \
              -v repl_file="$CPP.newblock" \
              -f "$ENGINE_AWK" "$CPP.step1" > "$CPP.step2" 2>"$CPP.err"; then
        cat "$CPP.err" >&2
        rm -f "$CPP.newblock" "$CPP.step1" "$CPP.step2" "$CPP.err"
        exit 1
    fi
    rm -f "$CPP.newblock" "$CPP.step1" "$CPP.err"

    if [[ "$DRY_RUN" -eq 0 ]]; then
        mv "$CPP.step2" "$CPP"
    else
        rm -f "$CPP.step2"
    fi
}

patch_header
patch_cpp

rm -f "$ENGINE_AWK"

if [[ "$DRY_RUN" -eq 1 ]]; then
    echo
    echo "Dry run only -- no files were written. Re-run without --dry-run to apply."
else
    echo
    echo "Done. Rebuild, then sanity-check with:"
    echo '  HINT=$(grep -o '"'"'mic_name_hint = "[^"]*"'"'"' include/audio_graph_manager.h | head -1 | cut -d\" -f2)'
    echo '  pactl list short sources | grep "$HINT"   # confirms the substring matches a real capture source'
    echo "Then press dry-mix up/down and watch:"
    echo "  watch -n0.5 pactl list sources   # or: pactl get-source-volume <source-name>"
fi
