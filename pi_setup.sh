#!/usr/bin/env bash
# pi_setup.sh
#
# Run this ON THE RASPBERRY PI, after running pi_audio_diagnose.sh and
# getting the real playback_FL / playback_FR port names.
#
# What it does:
#   1. Patches src/audio_graph_manager.cpp: replaces every hardcoded
#      Ubuntu-desktop fallback sink name with your real Pi port names.
#   2. Rebuilds the project.
#   3. Installs udev rules so the Vial HID device and ID75 keyboards
#      are accessible without a logged-in desktop session.
#   4. Installs the instrument as a systemd service (NOT auto-enabled --
#      it's only started when boot mode is "headless", see below).
#   5. Installs a dual-boot-mode system:
#        - a marker file on the boot partition: headless or normal
#        - a oneshot service that reads it at every boot and either
#          (a) headless: rfkill-blocks Wi-Fi + Bluetooth and starts the
#              instrument, or
#          (b) normal: leaves everything alone -- boots like a stock Pi,
#              instrument NOT auto-started.
#        - a set-boot-mode.sh helper to flip the mode for next boot, or
#          apply it live with --now, no reboot required.
#      Ethernet is never touched by any of this. If the marker file is
#      ever missing/unreadable, it defaults to "normal" -- you can never
#      get permanently locked out via this mechanism.
#
# Usage:
#   sudo ./pi_setup.sh --repo /home/pi/microtonal_instrument \
#       --left  "alsa_output.XXXX:playback_FL" \
#       --right "alsa_output.XXXX:playback_FR" \
#       [--default-mode headless|normal] \
#       [--skip-build] [--skip-service] [--skip-bootmode]
#
# Safe to re-run: each step checks before it changes anything, and the
# source file is backed up before editing.

set -euo pipefail

REPO=""
LEFT=""
RIGHT=""
DEFAULT_MODE="headless"
DO_BUILD=1
DO_SERVICE=1
DO_BOOTMODE=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo) REPO="$2"; shift 2 ;;
        --left) LEFT="$2"; shift 2 ;;
        --right) RIGHT="$2"; shift 2 ;;
        --default-mode) DEFAULT_MODE="$2"; shift 2 ;;
        --skip-build) DO_BUILD=0; shift ;;
        --skip-service) DO_SERVICE=0; shift ;;
        --skip-bootmode) DO_BOOTMODE=0; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

if [[ $EUID -ne 0 ]]; then
    echo "Please run this with sudo: sudo $0 --repo ... --left ... --right ..." >&2
    exit 1
fi

if [[ -z "$REPO" || -z "$LEFT" || -z "$RIGHT" ]]; then
    echo "Usage: sudo $0 --repo /path/to/repo --left <port_FL> --right <port_FR>" >&2
    exit 1
fi

if [[ "$DEFAULT_MODE" != "headless" && "$DEFAULT_MODE" != "normal" ]]; then
    echo "--default-mode must be 'headless' or 'normal'" >&2
    exit 1
fi

REPO="$(realpath "$REPO")"
SRC_FILE="$REPO/src/audio_graph_manager.cpp"

if [[ ! -f "$SRC_FILE" ]]; then
    echo "ERROR: $SRC_FILE not found. Is --repo pointing at the right directory?" >&2
    exit 1
fi

REAL_USER="${SUDO_USER:-$(logname 2>/dev/null || echo root)}"

echo "=================================================================="
echo " 1/5: Patching hardcoded audio sink in audio_graph_manager.cpp"
echo "=================================================================="

OLD_L="alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FL"
OLD_R="alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FR"

BACKUP="${SRC_FILE}.bak.pisetup.$(date +%Y%m%d_%H%M%S)"
cp "$SRC_FILE" "$BACKUP"
echo "Backed up original to: $BACKUP"

python3 - "$SRC_FILE" "$OLD_L" "$LEFT" "$OLD_R" "$RIGHT" <<'PYEOF'
import sys
path, old_l, new_l, old_r, new_r = sys.argv[1:6]
with open(path, "r") as f:
    text = f.read()

count_l = text.count(old_l)
count_r = text.count(old_r)
text = text.replace(old_l, new_l)
text = text.replace(old_r, new_r)

with open(path, "w") as f:
    f.write(text)

print(f"Replaced {count_l} occurrence(s) of the LEFT fallback sink.")
print(f"Replaced {count_r} occurrence(s) of the RIGHT fallback sink.")
if count_l == 0 and count_r == 0:
    print("WARNING: nothing matched -- the old strings may already have "
          "been changed, or this isn't the file you think it is. Check "
          "the backup and diff manually.")
PYEOF

echo
echo "=================================================================="
echo " 2/5: Rebuilding"
echo "=================================================================="
if [[ "$DO_BUILD" -eq 1 ]]; then
    su "$REAL_USER" -c "
        set -e
        mkdir -p '$REPO/build'
        cd '$REPO/build'
        cmake -DCMAKE_BUILD_TYPE=Release ..
        make -j\$(nproc)
    "
    echo "Build complete: $REPO/build/microtonal_instrument"
else
    echo "Skipped (--skip-build). Remember to rebuild before the service will pick up the change."
fi

echo
echo "=================================================================="
echo " 3/5: udev rules (HID + input access without a desktop session)"
echo "=================================================================="
UDEV_RULES_FILE="/etc/udev/rules.d/99-microtonal-instrument.rules"
cat > "$UDEV_RULES_FILE" <<'EOF'
# Vial-compatible HID raw interface (ID75 keyboards), VID 0x6964 PID 0x0075
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="6964", ATTRS{idProduct}=="0075", MODE="0666"
# ID75 keyboard event devices (evdev)
SUBSYSTEM=="input", ATTRS{idVendor}=="6964", ATTRS{idProduct}=="0075", MODE="0666"
EOF
udevadm control --reload-rules
udevadm trigger
echo "Installed $UDEV_RULES_FILE"
echo "(The service runs as root regardless of mode, so this isn't strictly"
echo " required -- it's here in case you ever run this as a normal user.)"

echo
echo "=================================================================="
echo " 4/5: systemd service (installed, but only auto-started in"
echo "      headless mode -- see step 5)"
echo "=================================================================="
if [[ "$DO_SERVICE" -eq 1 ]]; then
    SERVICE_FILE="/etc/systemd/system/microtonal-instrument.service"
    BIN_PATH="$REPO/build/microtonal_instrument"
    cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=Microtonal Instrument Engine
After=sound.target

[Service]
Type=simple
WorkingDirectory=$REPO
ExecStart=$BIN_PATH
Restart=on-failure
RestartSec=2
# Running as root avoids any /dev/hidraw*, /dev/input/event* permission
# issues in a headless boot with no logged-in desktop session.
User=root
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    echo "Installed $SERVICE_FILE"
    echo "NOT enabled for automatic start -- the boot-mode selector (step 5)"
    echo "starts it only when boot mode is 'headless'."
    echo "You can always start/stop it by hand:"
    echo "  sudo systemctl start microtonal-instrument"
    echo "  sudo systemctl stop microtonal-instrument"
    echo "  journalctl -u microtonal-instrument -f"
else
    echo "Skipped (--skip-service)."
fi

echo
echo "=================================================================="
echo " 5/5: Dual boot-mode system (headless vs normal)"
echo "=================================================================="
if [[ "$DO_BOOTMODE" -eq 1 ]]; then
    if [[ -d /boot/firmware ]]; then
        MODE_FILE="/boot/firmware/instrument-boot-mode"
    else
        MODE_FILE="/boot/instrument-boot-mode"
    fi

    if [[ ! -f "$MODE_FILE" ]]; then
        echo "$DEFAULT_MODE" > "$MODE_FILE"
        echo "Created $MODE_FILE with default mode: $DEFAULT_MODE"
    else
        echo "$MODE_FILE already exists (contains '$(cat "$MODE_FILE")') -- left as-is."
    fi

    cat > /usr/local/bin/microtonal-bootmode.sh <<EOF
#!/usr/bin/env bash
# Runs once at every boot. Reads the mode marker file and either
# rfkill-blocks Wi-Fi/Bluetooth + starts the instrument (headless), or
# leaves everything alone (normal / missing / unreadable file).
set -uo pipefail

MODE_FILE="$MODE_FILE"
MODE="normal"
if [[ -f "\$MODE_FILE" ]]; then
    MODE="\$(tr -d '[:space:]' < "\$MODE_FILE" | tr '[:upper:]' '[:lower:]')"
fi

logger -t microtonal-bootmode "Boot mode: \${MODE:-normal}"

if [[ "\$MODE" == "headless" ]]; then
    rfkill block wifi 2>/dev/null || true
    rfkill block bluetooth 2>/dev/null || true
    systemctl stop bluetooth.service 2>/dev/null || true
    systemctl start microtonal-instrument.service
else
    rfkill unblock wifi 2>/dev/null || true
    rfkill unblock bluetooth 2>/dev/null || true
fi
EOF
    chmod +x /usr/local/bin/microtonal-bootmode.sh

    cat > /etc/systemd/system/microtonal-bootmode.service <<EOF
[Unit]
Description=Microtonal Instrument boot-mode selector (headless vs normal)
After=local-fs.target
Before=bluetooth.service hciuart.service wpa_supplicant.service NetworkManager.service microtonal-instrument.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/bin/microtonal-bootmode.sh

[Install]
WantedBy=multi-user.target
EOF

    cat > /usr/local/bin/set-boot-mode.sh <<EOF
#!/usr/bin/env bash
set -euo pipefail
MODE_FILE="$MODE_FILE"

usage() {
    cat <<USAGE
Usage: sudo set-boot-mode.sh <headless|normal> [--now]

  headless   Next boot disables Wi-Fi/Bluetooth and auto-starts the
             instrument. Stays this way on every future boot until
             you change it again.
  normal     Next boot behaves like a stock Pi -- Wi-Fi/Bluetooth on,
             instrument NOT auto-started.
  --now      Also apply the change immediately, without rebooting.
USAGE
    exit 1
}

[[ \$EUID -eq 0 ]] || { echo "Run with sudo." >&2; exit 1; }
[[ \$# -ge 1 ]] || usage
MODE="\$1"
[[ "\$MODE" == "headless" || "\$MODE" == "normal" ]] || usage
APPLY_NOW=0
[[ "\${2:-}" == "--now" ]] && APPLY_NOW=1

echo "\$MODE" > "\$MODE_FILE"
echo "Boot mode set to '\$MODE' (takes effect next boot: \$MODE_FILE)"

if [[ "\$APPLY_NOW" -eq 1 ]]; then
    if [[ "\$MODE" == "headless" ]]; then
        rfkill block wifi 2>/dev/null || true
        rfkill block bluetooth 2>/dev/null || true
        systemctl stop bluetooth.service 2>/dev/null || true
        systemctl start microtonal-instrument.service
        echo "Applied now: Wi-Fi/Bluetooth blocked, instrument started."
    else
        systemctl stop microtonal-instrument.service 2>/dev/null || true
        rfkill unblock wifi 2>/dev/null || true
        rfkill unblock bluetooth 2>/dev/null || true
        systemctl start bluetooth.service 2>/dev/null || true
        echo "Applied now: instrument stopped, Wi-Fi/Bluetooth unblocked."
    fi
fi
EOF
    chmod +x /usr/local/bin/set-boot-mode.sh

    systemctl daemon-reload
    systemctl enable microtonal-bootmode.service

    echo "Installed:"
    echo "  $MODE_FILE               (the mode marker -- FAT partition,"
    echo "                              editable even from another computer"
    echo "                              if the Pi is ever unreachable)"
    echo "  /usr/local/bin/microtonal-bootmode.sh   (runs at every boot)"
    echo "  /usr/local/bin/set-boot-mode.sh         (your escape hatch)"
    echo
    echo "Ethernet is never touched by any of this -- only Wi-Fi/Bluetooth"
    echo "radios via rfkill. If $MODE_FILE is ever missing or unreadable,"
    echo "boot defaults to normal, so this can't permanently lock you out."
else
    echo "Skipped (--skip-bootmode)."
fi

echo
echo "=================================================================="
echo " Done."
echo "=================================================================="
echo "Summary:"
echo "  - Audio sink fallback patched to your real Pi ports."
echo "  - Rebuilt: $([[ $DO_BUILD -eq 1 ]] && echo yes || echo skipped)"
echo "  - udev rules installed."
echo "  - systemd service installed: $([[ $DO_SERVICE -eq 1 ]] && echo yes || echo skipped)"
echo "  - Dual boot-mode installed: $([[ $DO_BOOTMODE -eq 1 ]] && echo "yes (default: $DEFAULT_MODE)" || echo skipped)"
echo
echo "To change mode:      sudo set-boot-mode.sh headless   (or normal)"
echo "To change mode NOW:  sudo set-boot-mode.sh headless --now"
echo "Reboot to test:      sudo reboot"
