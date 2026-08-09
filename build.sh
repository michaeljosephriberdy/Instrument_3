#!/usr/bin/env bash
# build.sh
#
# Normal use -- unchanged from today:
#   ./build.sh
#       Just configures + builds the project. Touches nothing else.
#
# Field-deploy use -- run once per machine, after running
# pi_audio_diagnose.sh to get your real port names:
#   sudo ./build.sh --headless-boot \
#       --audio-left  "alsa_output.XXXX:playback_FL" \
#       --audio-right "alsa_output.XXXX:playback_FR" \
#       [--timeout 4]
#
#   This will:
#     1. One-time codemod (idempotent, safe to run again): turns the
#        hardcoded Ubuntu-desktop fallback sink names in
#        audio_graph_manager.cpp into CMake-overridable macros
#        (PI_AUDIO_LEFT_SINK / PI_AUDIO_RIGHT_SINK), and adds the
#        override block to CMakeLists.txt. A plain ./build.sh with no
#        flags is unaffected by this -- the macros default back to the
#        original Ubuntu fallback string when not overridden.
#     2. Configures + builds with your real Pi port names baked in via
#        -DPI_AUDIO_LEFT_SINK=... -DPI_AUDIO_RIGHT_SINK=...
#     3. Installs udev rules (HID/input access without a desktop session).
#     4. Installs the instrument as a systemd service (not auto-enabled --
#        only the boot menu below starts it).
#     5. Installs a boot-time console menu:
#          - Every boot: short countdown ("booting headless in 4s...").
#          - No input (typical on-the-road boot, nothing attached):
#            Wi-Fi/Bluetooth blocked via rfkill, instrument auto-starts.
#          - Esc pressed during countdown (monitor+keyboard attached
#            for debugging): boots the normal OS -- Wi-Fi/Bluetooth on,
#            instrument NOT auto-started.
#     Ethernet is never touched by any of this.
#
# Undo / factory-reset-style cleanup:
#   sudo ./build.sh --uninstall-headless-boot
#       Removes the boot menu + instrument service + udev rule, and
#       unblocks Wi-Fi/Bluetooth right now. Repo/source is left as-is
#       (the codemod is harmless and doesn't need reverting).
#
# Every mode (including a plain ./build.sh) checks for required apt
# packages first and installs anything missing. Pass
# --skip-package-check to skip this (e.g. offline, or non-apt system).

set -euo pipefail

HEADLESS_BOOT=0
UNINSTALL=0
AUDIO_LEFT=""
AUDIO_RIGHT=""
TIMEOUT=4
SKIP_PACKAGE_CHECK=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --headless-boot) HEADLESS_BOOT=1; shift ;;
        --uninstall-headless-boot) UNINSTALL=1; shift ;;
        --audio-left) AUDIO_LEFT="$2"; shift 2 ;;
        --audio-right) AUDIO_RIGHT="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --skip-package-check) SKIP_PACKAGE_CHECK=1; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$SCRIPT_DIR"
CMAKE_FILE="$REPO/CMakeLists.txt"
SRC_FILE="$REPO/src/audio_graph_manager.cpp"
TARGET_NAME="microtonal_instrument"

# ------------------------------------------------------------------
# Package check + install (runs before everything except --uninstall)
# ------------------------------------------------------------------
check_and_install_packages() {
    if [[ "$SKIP_PACKAGE_CHECK" -eq 1 ]]; then
        return
    fi
    if ! command -v apt-get >/dev/null 2>&1; then
        echo "=== Package check: apt-get not found, skipping (not a Debian/Ubuntu system?) ==="
        return
    fi

    echo "=== Package check ==="

    # Needed to configure/compile the project.
    local build_pkgs=(build-essential cmake pkg-config git libasound2-dev libhidapi-dev)
    # Core PipeWire graph tooling the code shells out to: pw-link/pw-cli/pw-jack
    # (pipewire-bin), wpctl (wireplumber), pactl (pulseaudio-utils, against the
    # pipewire-pulse compat socket), plus amixer/aplay (alsa-utils) and rfkill.
    local pipewire_pkgs=(pipewire pipewire-bin pipewire-audio-client-libraries \
        pipewire-pulse wireplumber pulseaudio-utils alsa-utils rfkill)
    # The actual audio engines the graph launches/controls at runtime --
    # confirmed against every which()/popen() call in audio_graph_manager.cpp
    # and docs/phase4_packages.md: ZynAddSubFX (synth), SooperLooper (looper),
    # jalv + calf-plugins (LV2 host + Calf Vocoder), liblo-tools (oscsend, used
    # to drive SooperLooper over OSC).
    local audio_app_pkgs=(zynaddsubfx sooperlooper calf-plugins jalv liblo-tools)
    local all_pkgs=("${build_pkgs[@]}" "${pipewire_pkgs[@]}" "${audio_app_pkgs[@]}")

    local to_install=()
    local unknown=()

    for pkg in "${all_pkgs[@]}"; do
        if dpkg -s "$pkg" >/dev/null 2>&1; then
            continue
        fi
        if apt-cache show "$pkg" >/dev/null 2>&1; then
            to_install+=("$pkg")
        else
            unknown+=("$pkg")
        fi
    done

    if [[ ${#unknown[@]} -gt 0 ]]; then
        echo "Note: these package names weren't found in apt on this system"
        echo "(naming can differ by OS/version) -- check manually if needed:"
        printf '  %s\n' "${unknown[@]}"
    fi

    if [[ ${#to_install[@]} -eq 0 ]]; then
        echo "All known required packages already installed."
        return
    fi

    echo "Installing missing packages: ${to_install[*]}"
    if [[ $EUID -eq 0 ]]; then
        apt-get update
        apt-get install -y "${to_install[@]}"
    else
        sudo apt-get update
        sudo apt-get install -y "${to_install[@]}"
    fi
}

if [[ "$UNINSTALL" -eq 0 ]]; then
    check_and_install_packages
fi

# ------------------------------------------------------------------
# --uninstall-headless-boot
# ------------------------------------------------------------------
if [[ "$UNINSTALL" -eq 1 ]]; then
    if [[ $EUID -ne 0 ]]; then
        echo "Please run with sudo: sudo $0 --uninstall-headless-boot" >&2
        exit 1
    fi
    systemctl disable --now microtonal-bootmenu.service 2>/dev/null || true
    systemctl disable --now microtonal-instrument.service 2>/dev/null || true
    rm -f /etc/systemd/system/microtonal-bootmenu.service
    rm -f /etc/systemd/system/microtonal-instrument.service
    rm -f /usr/local/bin/microtonal-bootmenu.sh
    rm -f /etc/udev/rules.d/99-microtonal-instrument.rules
    udevadm control --reload-rules 2>/dev/null || true
    systemctl daemon-reload
    rfkill unblock wifi 2>/dev/null || true
    rfkill unblock bluetooth 2>/dev/null || true
    echo "Headless boot mode removed. Wi-Fi/Bluetooth unblocked now."
    echo "The source codemod (macro-ized sink names) was left in place --"
    echo "it's harmless and a plain ./build.sh behaves exactly as before."
    exit 0
fi

# ------------------------------------------------------------------
# --headless-boot validation
# ------------------------------------------------------------------
if [[ "$HEADLESS_BOOT" -eq 1 ]]; then
    if [[ $EUID -ne 0 ]]; then
        echo "Please run with sudo: sudo $0 --headless-boot --audio-left ... --audio-right ..." >&2
        exit 1
    fi
    if [[ -z "$AUDIO_LEFT" || -z "$AUDIO_RIGHT" ]]; then
        echo "ERROR: --headless-boot requires --audio-left and --audio-right." >&2
        echo "Run pi_audio_diagnose.sh first to find the real port names." >&2
        exit 1
    fi
fi

REAL_USER="${SUDO_USER:-$(logname 2>/dev/null || echo "$(id -un)")}"

# ------------------------------------------------------------------
# Step: one-time codemod (only runs with --headless-boot)
# ------------------------------------------------------------------
if [[ "$HEADLESS_BOOT" -eq 1 ]]; then
    echo "=== Codemod: making the audio sink overridable ==="

    cp "$SRC_FILE" "${SRC_FILE}.bak.$(date +%Y%m%d_%H%M%S)"
    cp "$CMAKE_FILE" "${CMAKE_FILE}.bak.$(date +%Y%m%d_%H%M%S)"

    python3 - "$SRC_FILE" <<'PYEOF'
import sys
path = sys.argv[1]
with open(path) as f:
    text = f.read()

MARKER = "PI_AUDIO_LEFT_SINK"
if MARKER not in text:
    lines = text.split("\n")
    include_idxs = [i for i, l in enumerate(lines) if l.strip().startswith("#include")]
    insert_at = (max(include_idxs) + 1) if include_idxs else 0
    block = [
        "",
        "#ifndef PI_AUDIO_LEFT_SINK",
        '#define PI_AUDIO_LEFT_SINK "alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FL"',
        "#endif",
        "#ifndef PI_AUDIO_RIGHT_SINK",
        '#define PI_AUDIO_RIGHT_SINK "alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FR"',
        "#endif",
        "",
    ]
    lines[insert_at:insert_at] = block
    text = "\n".join(lines)
    print("audio_graph_manager.cpp: inserted macro fallback block")
else:
    print("audio_graph_manager.cpp: macro fallback block already present")

old_l = '"alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FL"'
old_r = '"alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FR"'
count_l = text.count(old_l)
count_r = text.count(old_r)
text = text.replace(old_l, "PI_AUDIO_LEFT_SINK")
text = text.replace(old_r, "PI_AUDIO_RIGHT_SINK")

with open(path, "w") as f:
    f.write(text)
print(f"audio_graph_manager.cpp: replaced {count_l} LEFT / {count_r} RIGHT literal(s) with macros")
PYEOF

    python3 - "$CMAKE_FILE" "$TARGET_NAME" <<'PYEOF'
import sys
path, target = sys.argv[1:3]
with open(path) as f:
    text = f.read()

MARKER = "PI_AUDIO_LEFT_SINK"
if MARKER not in text:
    block = f"""
# --- Pi headless audio sink overrides (added by build.sh --headless-boot) ---
if(DEFINED PI_AUDIO_LEFT_SINK)
    target_compile_definitions({target} PRIVATE PI_AUDIO_LEFT_SINK="${{PI_AUDIO_LEFT_SINK}}")
endif()
if(DEFINED PI_AUDIO_RIGHT_SINK)
    target_compile_definitions({target} PRIVATE PI_AUDIO_RIGHT_SINK="${{PI_AUDIO_RIGHT_SINK}}")
endif()
"""
    text = text.rstrip("\n") + "\n" + block
    with open(path, "w") as f:
        f.write(text)
    print("CMakeLists.txt: added override block")
else:
    print("CMakeLists.txt: override block already present, skipped")
PYEOF
    echo
fi

# ------------------------------------------------------------------
# Step: configure + build (always runs; as the real user, not root)
# ------------------------------------------------------------------
echo "=== Building ==="
CMAKE_EXTRA_ARGS=()
if [[ "$HEADLESS_BOOT" -eq 1 ]]; then
    CMAKE_EXTRA_ARGS+=("-DPI_AUDIO_LEFT_SINK=${AUDIO_LEFT}" "-DPI_AUDIO_RIGHT_SINK=${AUDIO_RIGHT}")
fi

BUILD_CMD="mkdir -p '$REPO/build' && cd '$REPO/build' && cmake -DCMAKE_BUILD_TYPE=Release ${CMAKE_EXTRA_ARGS[*]@Q} .. && make -j\$(nproc)"

if [[ $EUID -eq 0 ]]; then
    su "$REAL_USER" -c "$BUILD_CMD"
else
    bash -c "$BUILD_CMD"
fi
echo "Build complete: $REPO/build/$TARGET_NAME"

if [[ "$HEADLESS_BOOT" -eq 0 ]]; then
    exit 0
fi

# ------------------------------------------------------------------
# Step: udev rules
# ------------------------------------------------------------------
echo
echo "=== udev rules ==="
cat > /etc/udev/rules.d/99-microtonal-instrument.rules <<'EOF'
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="6964", ATTRS{idProduct}=="0075", MODE="0666"
SUBSYSTEM=="input", ATTRS{idVendor}=="6964", ATTRS{idProduct}=="0075", MODE="0666"
EOF
udevadm control --reload-rules
udevadm trigger
echo "Installed 99-microtonal-instrument.rules"

# ------------------------------------------------------------------
# Step: instrument systemd service (installed, not enabled)
# ------------------------------------------------------------------
echo
echo "=== Instrument systemd service ==="
BIN_PATH="$REPO/build/$TARGET_NAME"
cat > /etc/systemd/system/microtonal-instrument.service <<EOF
[Unit]
Description=Microtonal Instrument Engine
After=sound.target

[Service]
Type=simple
WorkingDirectory=$REPO
ExecStart=$BIN_PATH
Restart=on-failure
RestartSec=2
User=root
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
echo "Installed (not enabled -- the boot menu below starts it conditionally)."

# ------------------------------------------------------------------
# Step: boot menu (countdown + Esc-to-cancel)
# ------------------------------------------------------------------
echo
echo "=== Boot menu (headless by default, Esc for normal boot) ==="
cat > /usr/local/bin/microtonal-bootmenu.sh <<EOF
#!/usr/bin/env bash
set -uo pipefail
TIMEOUT=$TIMEOUT

echo ""
echo "=== Microtonal Instrument ==="
echo "Booting headless in \${TIMEOUT}s (Wi-Fi/Bluetooth off, instrument auto-starts)."
echo "Press Esc now to boot the normal OS instead."

KEY=""
IFS= read -rsn1 -t "\$TIMEOUT" KEY || true

if [[ "\$KEY" == \$'\\e' ]]; then
    echo "Esc received -- booting normal OS."
    echo "Wi-Fi/Bluetooth stay on; instrument NOT auto-started."
    echo "(start it by hand: sudo systemctl start microtonal-instrument)"
    exit 0
fi

echo "Starting headless mode..."
rfkill block wifi 2>/dev/null || true
rfkill block bluetooth 2>/dev/null || true
systemctl stop bluetooth.service 2>/dev/null || true
systemctl start microtonal-instrument.service
EOF
chmod +x /usr/local/bin/microtonal-bootmenu.sh

cat > /etc/systemd/system/microtonal-bootmenu.service <<'EOF'
[Unit]
Description=Microtonal Instrument boot menu (headless by default, Esc for normal boot)
After=local-fs.target
Before=getty@tty1.service bluetooth.service hciuart.service wpa_supplicant.service NetworkManager.service microtonal-instrument.service
Conflicts=getty@tty1.service

[Service]
Type=oneshot
RemainAfterExit=no
StandardInput=tty
StandardOutput=tty
TTYPath=/dev/tty1
TTYReset=yes
TTYVHangup=yes
ExecStart=/usr/local/bin/microtonal-bootmenu.sh

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable microtonal-bootmenu.service
echo "Installed and enabled. Countdown: ${TIMEOUT}s. Ethernet is never touched."

echo
echo "=================================================================="
echo " Done. Reboot to test: sudo reboot"
echo "=================================================================="
echo "  - No input at boot            -> headless, instrument auto-starts"
echo "  - Esc pressed during countdown -> normal OS, Wi-Fi/Bluetooth on"
echo "  - To undo everything:  sudo ./build.sh --uninstall-headless-boot"
