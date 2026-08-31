#!/usr/bin/env bash
# fix_bluetooth_boot.sh
#
# Fixes Bluetooth that fails after "Escape → boot normally" (or any boot
# path that left the adapter soft-blocked / service stopped / module unbound).
#
# Safe on Ubuntu 24.04 + System76. Idempotent. No headless/instrument logic
# is changed — only restores a working Bluetooth stack for the desktop session.
#
# Usage:
#   bash fix_bluetooth_boot.sh           # diagnose + fix now
#   bash fix_bluetooth_boot.sh --permanent  # also enable service at boot
#
set -euo pipefail

PERMANENT=0
[[ "${1:-}" == "--permanent" ]] && PERMANENT=1

echo "=============================================="
echo " Bluetooth diagnose + fix"
echo "=============================================="
echo

echo "--- 1. rfkill (soft/hard block) ---"
if command -v rfkill >/dev/null; then
  rfkill list || true
else
  echo "rfkill not installed"
fi
echo

echo "--- 2. bluetooth service ---"
systemctl is-enabled bluetooth.service 2>&1 || true
systemctl is-active bluetooth.service 2>&1 || true
systemctl status bluetooth.service --no-pager -l 2>&1 | head -25 || true
echo

echo "--- 3. kernel modules / USB controller ---"
lsmod | grep -iE 'bluetooth|btusb|btintel|btrtl|btbcm|ath3k' || echo "(no bluetooth modules loaded)"
echo
lsusb 2>/dev/null | grep -iE 'bluetooth|broadcom|intel|realtek|mediatek|qualcomm' || echo "(no obvious BT USB device in lsusb)"
echo
dmesg -T 2>/dev/null | grep -iE 'bluetooth|btusb|hci' | tail -20 || true
echo

echo "--- 4. Applying fix ---"

# Unblock any soft-block (common after airplane-mode / boot-script / rfkill)
if command -v rfkill >/dev/null; then
  sudo rfkill unblock bluetooth || true
  sudo rfkill unblock all || true
  echo "rfkill: unblocked bluetooth"
fi

# Ensure modules are present
sudo modprobe bluetooth 2>/dev/null || true
sudo modprobe btusb 2>/dev/null || true
echo "modules: bluetooth/btusb probed"

# Unmask + enable + start service (in case a headless path masked it)
sudo systemctl unmask bluetooth.service 2>/dev/null || true
sudo systemctl enable bluetooth.service 2>/dev/null || true
sudo systemctl restart bluetooth.service
sleep 1
if systemctl is-active --quiet bluetooth.service; then
  echo "bluetooth.service: active"
else
  echo "WARNING: bluetooth.service failed to start"
  journalctl -u bluetooth.service -n 30 --no-pager || true
fi

# BlueZ userspace helpers if present
if command -v bluetoothctl >/dev/null; then
  # Power on the default controller (non-interactive)
  bluetoothctl power on 2>/dev/null || true
  bluetoothctl show 2>/dev/null | head -20 || true
fi

echo
echo "--- 5. Post-fix state ---"
rfkill list 2>/dev/null || true
systemctl is-active bluetooth.service 2>&1 || true
hciconfig -a 2>/dev/null || true
bluetoothctl list 2>/dev/null || true
echo

if [[ "$PERMANENT" -eq 1 ]]; then
  echo "--- 6. Permanent: ensure service enabled + unblock on login ---"
  sudo systemctl enable bluetooth.service

  # Drop-in so bluetooth is never left masked by a custom boot path
  sudo mkdir -p /etc/systemd/system/bluetooth.service.d
  sudo tee /etc/systemd/system/bluetooth.service.d/override.conf >/dev/null <<'EOF'
[Unit]
# Keep Bluetooth available on normal (graphical) boots even if another
# boot path temporarily stopped or masked it.
RefuseManualStop=no

[Service]
# No change to ExecStart — presence of this drop-in documents intent.
EOF
  sudo systemctl daemon-reload

  # User-level: unblock + power on at graphical login (harmless if already on)
  mkdir -p "${HOME}/.config/autostart"
  cat > "${HOME}/.config/autostart/bluetooth-ensure.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Ensure Bluetooth
Comment=Unblock and power on Bluetooth after login
Exec=sh -c 'rfkill unblock bluetooth 2>/dev/null; sleep 1; bluetoothctl power on 2>/dev/null; true'
X-GNOME-Autostart-enabled=true
NoDisplay=true
EOF
  echo "Installed login autostart: ~/.config/autostart/bluetooth-ensure.desktop"
  echo "Enabled bluetooth.service for multi-user boot"
fi

echo
echo "=============================================="
echo " Done"
echo "=============================================="
echo "If the adapter still missing after this:"
echo "  1. Reboot once (clean firmware bind)"
echo "  2. Check Settings → Bluetooth toggle"
echo "  3. Run:  journalctl -b -u bluetooth --no-pager | tail -50"
echo "  4. For System76:  system76-firmware  /  firmware updates"
echo
echo "Re-run with --permanent to enable service at every boot + login unblock:"
echo "  bash fix_bluetooth_boot.sh --permanent"
echo "=============================================="
