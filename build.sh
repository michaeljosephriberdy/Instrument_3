#!/usr/bin/env bash
# build.sh -- builds the instrument for the laptop or the Pi.
#
#   ./build.sh                        # remembered platform (asks the first time)
#   ./build.sh --platform laptop      # built-in headphones ports, never autostarts
#   ./build.sh --platform pi          # Shure MVX2U output; also asks about start-at-boot
#   ./build.sh --platform pi --autostart yes|no
#   ./build.sh --platform pi --audio-left "alsa_output.XXXX:playback_FL" \
#                            --audio-right "alsa_output.XXXX:playback_FR"
#   ./build.sh --reset-platform       # forget the remembered answers
#   ./build.sh --skip-package-check   # don't check/install apt packages
#
# There is one boot behaviour only: on the Pi, optionally "start the instrument
# at boot". There is no headless/normal boot menu any more.
set -euo pipefail

PLATFORM=""
AUDIO_LEFT=""
AUDIO_RIGHT=""
AUTOSTART=""
SKIP_PKG=0
RESET=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --platform)    PLATFORM="${2:-}"; shift 2 ;;
        --audio-left)  AUDIO_LEFT="${2:-}"; shift 2 ;;
        --audio-right) AUDIO_RIGHT="${2:-}"; shift 2 ;;
        --autostart)   AUTOSTART="${2:-}"; shift 2 ;;
        --skip-package-check) SKIP_PKG=1; shift ;;
        --reset-platform) RESET=1; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$REPO/build"
STATE_FILE="$REPO/.build_platform"
LAPTOP_LEFT="alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FL"
LAPTOP_RIGHT="alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FR"
UNIT_NAME="microtonal-instrument.service"
UNIT_DIR="$HOME/.config/systemd/user"

[[ $RESET -eq 1 ]] && rm -f "$STATE_FILE"

# ---- packages ---------------------------------------------------------------
check_packages() {
    [[ $SKIP_PKG -eq 1 ]] && return 0
    command -v apt-get >/dev/null 2>&1 || return 0
    local pkgs=(build-essential cmake pkg-config git libasound2-dev libhidapi-dev
                pipewire pipewire-bin pipewire-jack pipewire-audio-client-libraries
                pipewire-pulse wireplumber pulseaudio-utils alsa-utils rfkill
                zynaddsubfx sooperlooper calf-plugins jalv lv2-utils lilv-utils liblo-tools)
    local missing=() p
    for p in "${pkgs[@]}"; do
        dpkg -s "$p" >/dev/null 2>&1 && continue
        apt-cache show "$p" >/dev/null 2>&1 && missing+=("$p")
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "Installing missing packages: ${missing[*]}"
        sudo apt-get update
        sudo apt-get install -y "${missing[@]}"
    fi
}
check_packages

# ---- remembered answers -----------------------------------------------------
SAVED_PLATFORM=""; SAVED_LEFT=""; SAVED_RIGHT=""; SAVED_AUTOSTART=""
# shellcheck disable=SC1090
[[ -f "$STATE_FILE" ]] && source "$STATE_FILE"
if [[ -z "$PLATFORM" && -n "$SAVED_PLATFORM" ]]; then
    PLATFORM="$SAVED_PLATFORM"
    echo "Using remembered platform: $PLATFORM (change with --platform, or --reset-platform)"
fi

# ---- choose platform --------------------------------------------------------
if [[ -z "$PLATFORM" ]]; then
    if [[ -t 0 ]]; then
        read -r -p "Build for laptop or pi? [laptop/pi] (default laptop): " PLATFORM
    fi
    PLATFORM="${PLATFORM:-laptop}"
fi
PLATFORM="$(echo "$PLATFORM" | tr '[:upper:]' '[:lower:]')"
[[ "$PLATFORM" == "laptop" || "$PLATFORM" == "pi" ]] || { echo "Platform must be 'laptop' or 'pi'." >&2; exit 1; }

case "$PLATFORM" in
    laptop)
        AUDIO_LEFT="$LAPTOP_LEFT"
        AUDIO_RIGHT="$LAPTOP_RIGHT"
        AUTOSTART="no"
        ;;
    pi)
        [[ -n "$AUDIO_LEFT"  ]] || AUDIO_LEFT="$SAVED_LEFT"
        [[ -n "$AUDIO_RIGHT" ]] || AUDIO_RIGHT="$SAVED_RIGHT"
        if [[ -z "$AUDIO_LEFT" || -z "$AUDIO_RIGHT" ]] && command -v pw-link >/dev/null 2>&1; then
            # The Shure's playback ports appear as *input* ports in pw-link.
            det_l="$(pw-link -i 2>/dev/null | sed 's/^[[:space:]]*//' | grep -i 'MVX2U' | grep 'playback_FL' | head -n1 || true)"
            det_r="$(pw-link -i 2>/dev/null | sed 's/^[[:space:]]*//' | grep -i 'MVX2U' | grep 'playback_FR' | head -n1 || true)"
            if [[ -n "$det_l" && -n "$det_r" ]]; then
                AUDIO_LEFT="${AUDIO_LEFT:-$det_l}"; AUDIO_RIGHT="${AUDIO_RIGHT:-$det_r}"
                echo "Detected Shure MVX2U playback ports:"; echo "  L: $AUDIO_LEFT"; echo "  R: $AUDIO_RIGHT"
            fi
        fi
        if [[ -z "$AUDIO_LEFT" || -z "$AUDIO_RIGHT" ]] && [[ -t 0 ]]; then
            echo "Could not auto-detect the Shure ports (plugged in? PipeWire running?)."
            echo "Find them with:  pw-link -i | grep -i MVX2U"
            read -r -p "Left playback port  (...:playback_FL): " AUDIO_LEFT
            read -r -p "Right playback port (...:playback_FR): " AUDIO_RIGHT
        fi
        [[ -n "$AUDIO_LEFT" && -n "$AUDIO_RIGHT" ]] || {
            echo "ERROR: pi build needs --audio-left/--audio-right (or a detectable, plugged-in MVX2U)." >&2; exit 1; }
        if [[ -z "$AUTOSTART" ]]; then
            if [[ -t 0 ]]; then
                def="${SAVED_AUTOSTART:-no}"
                read -r -p "Start the instrument automatically when this Pi boots? [yes/no] (default $def): " AUTOSTART
                AUTOSTART="${AUTOSTART:-$def}"
            else
                AUTOSTART="${SAVED_AUTOSTART:-no}"
            fi
        fi
        case "$(echo "$AUTOSTART" | tr '[:upper:]' '[:lower:]')" in
            y|yes) AUTOSTART="yes" ;;
            *)     AUTOSTART="no" ;;
        esac
        ;;
esac

printf 'SAVED_PLATFORM=%q\nSAVED_LEFT=%q\nSAVED_RIGHT=%q\nSAVED_AUTOSTART=%q\n' \
    "$PLATFORM" "$AUDIO_LEFT" "$AUDIO_RIGHT" "$AUTOSTART" > "$STATE_FILE"

echo "Platform  : $PLATFORM"
echo "Left sink : $AUDIO_LEFT"
echo "Right sink: $AUDIO_RIGHT"
[[ "$PLATFORM" == "pi" ]] && echo "Autostart : $AUTOSTART"
echo

# ---- configure + build (incremental: build/ is never wiped) -----------------
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached_home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt")"
    if [[ -n "$cached_home" && "$cached_home" != "$REPO" ]]; then
        echo "build/ was configured for $cached_home; resetting its CMake cache."
        rm -f "$BUILD_DIR/CMakeCache.txt"; rm -rf "$BUILD_DIR/CMakeFiles"
    fi
fi

echo "Note: the first configure downloads nlohmann/json from GitHub, so the machine needs internet once."
JOBS="$(nproc)"
case "$(uname -m)" in
    aarch64|arm*)   # C++20 compiles can run a small board out of RAM: about 1 job per GB
        mem_gb="$(awk '/MemTotal/ {printf "%d", $2/1048576}' /proc/meminfo)"
        [[ "$mem_gb" -lt 1 ]] && mem_gb=1
        (( JOBS > mem_gb )) && JOBS="$mem_gb"
        ;;
esac

cmake -S "$REPO" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DPI_AUDIO_LEFT_SINK="$AUDIO_LEFT" \
    -DPI_AUDIO_RIGHT_SINK="$AUDIO_RIGHT"
cmake --build "$BUILD_DIR" -j"$JOBS"
echo
echo "Build complete: $BUILD_DIR/microtonal_instrument"

# ---- Pi: start at boot (systemd USER service + linger; no login needed) -----
remove_autostart() {
    rm -f "$UNIT_DIR/$UNIT_NAME" "$UNIT_DIR/default.target.wants/$UNIT_NAME"
    systemctl --user daemon-reload 2>/dev/null || true
}

install_autostart() {
    local me; me="$(id -un)"
    echo
    echo "=== Installing start-at-boot for user '$me' ==="
    # Keyboard/HID access without a desktop session.
    sudo tee /etc/udev/rules.d/99-microtonal-instrument.rules >/dev/null <<'RULES'
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="6964", ATTRS{idProduct}=="0075", MODE="0666"
SUBSYSTEM=="input", ATTRS{idVendor}=="6964", ATTRS{idProduct}=="0075", MODE="0666"
RULES
    sudo udevadm control --reload-rules || true
    sudo udevadm trigger || true
    # Audio / MIDI / input device groups (only ones that exist).
    local g
    for g in audio plugdev input; do
        getent group "$g" >/dev/null 2>&1 && sudo usermod -aG "$g" "$me" || true
    done
    # Let the user's PipeWire + this service start at boot with nobody logged in.
    sudo loginctl enable-linger "$me"

    mkdir -p "$UNIT_DIR/default.target.wants"
    cat > "$UNIT_DIR/$UNIT_NAME" <<'UNITEOF'
[Unit]
Description=Microtonal Instrument Engine
After=pipewire.service wireplumber.service pipewire-pulse.service sound.target
Wants=pipewire.service wireplumber.service pipewire-pulse.service

[Service]
Type=simple
WorkingDirectory=@REPO@
# Wait (up to 60 s) for the CONFIGURED output (the Shure) to show up in PipeWire, then go.
TimeoutStartSec=120
ExecStartPre=/bin/bash -c 'for i in $$(seq 1 60); do pw-link -i 2>/dev/null | grep -qF "@LEFT@" && exit 0; sleep 1; done; exit 0'
ExecStart=@BIN@
Restart=on-failure
RestartSec=3
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=default.target
UNITEOF
    sed -i "s|@REPO@|$REPO|g; s|@BIN@|$BUILD_DIR/microtonal_instrument|g; s|@LEFT@|$AUDIO_LEFT|g" "$UNIT_DIR/$UNIT_NAME"
    ln -sf "../$UNIT_NAME" "$UNIT_DIR/default.target.wants/$UNIT_NAME"
    systemctl --user daemon-reload 2>/dev/null || true
    echo "Installed. It will start at every boot (no login, no password needed)."
    echo "  test now : systemctl --user start $UNIT_NAME"
    echo "  logs     : journalctl --user -u $UNIT_NAME -f     (and $REPO/instrument.log)"
    echo "  turn off : ./build.sh --platform pi --autostart no"
    echo "Group changes (audio/plugdev/input) apply from the next login/boot."
}

if [[ "$PLATFORM" == "pi" ]]; then
    if [[ "$AUTOSTART" == "yes" ]]; then install_autostart; else remove_autostart; fi
fi
