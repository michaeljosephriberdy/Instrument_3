#!/usr/bin/env bash
# pi_audio_diagnose.sh
#
# Run this ON THE RASPBERRY PI (after boot, with PipeWire/WirePlumber up,
# and with whatever output you care about already plugged in/powered on).
# It changes NOTHING -- it just prints everything needed to identify the
# exact PipeWire port names for the 3.5mm analog jack, so you can feed
# them into pi_setup.sh.
#
# Usage:  chmod +x pi_audio_diagnose.sh && ./pi_audio_diagnose.sh

set -uo pipefail

hr() { printf '%s\n' "------------------------------------------------------------"; }

echo "=== ALSA cards (kernel-level) ==="
cat /proc/asound/cards 2>/dev/null || echo "(no /proc/asound/cards -- is ALSA loaded?)"
hr

echo "=== aplay -l (ALSA playback devices) ==="
if command -v aplay >/dev/null; then
    aplay -l
else
    echo "(aplay not found -- sudo apt install -y alsa-utils)"
fi
hr

echo "=== PipeWire/WirePlumber user-service status ==="
systemctl --user is-active pipewire pipewire-pulse wireplumber 2>/dev/null \
    || echo "(one or more not active -- see 'systemctl --user status pipewire wireplumber')"
hr

echo "=== wpctl status (PipeWire graph overview) ==="
if command -v wpctl >/dev/null; then
    wpctl status
else
    echo "(wpctl not found -- sudo apt install -y wireplumber)"
fi
hr

echo "=== All PipeWire playback (input) ports -- pw-link -i ==="
if command -v pw-link >/dev/null; then
    pw-link -i
else
    echo "(pw-link not found -- sudo apt install -y pipewire-bin)"
fi
hr

echo "=== All PipeWire capture (output) ports -- pw-link -o ==="
pw-link -o 2>/dev/null
hr

echo "=== Likely candidates for the 3.5mm jack ==="
echo "(The Pi's onboard analog output is usually an ALSA card named"
echo " something like 'bcm2835 Headphones' or just 'Headphones', and"
echo " shows up as a PipeWire sink whose name contains that. HDMI"
echo " outputs contain 'HDMI' or 'vc4hdmi' -- ignore those. If you're"
echo " on a Pi 5, there IS NO analog jack -- you'll need a USB DAC or"
echo " HDMI audio instead, and this list will show you what's there.)"
echo
pw-link -i 2>/dev/null | grep -iE 'headphone|bcm2835|pcm|analog'
echo "(if nothing printed above, scroll up to the full 'pw-link -i' list"
echo " and look for the port names yourself -- naming varies by OS version)"
hr

echo "=== Raw node names/descriptions (pw-cli) ==="
if command -v pw-cli >/dev/null; then
    pw-cli list-objects 2>/dev/null | grep -iE 'node.name|node.description'
else
    echo "(pw-cli not found)"
fi
hr

cat <<'EOF'
WHAT TO DO WITH THIS OUTPUT
============================
1. In the "pw-link -i" list above, find the two ports for LEFT and
   RIGHT on the output you want. They'll look something like:

     alsa_output.platform-soc_audio.stereo-fallback:playback_FL
     alsa_output.platform-soc_audio.stereo-fallback:playback_FR

   (the middle part depends on your exact Pi model/OS -- that's why
   this has to be run on the Pi itself rather than guessed.)

2. Copy the full port name (the part BEFORE the colon) for each of
   playback_FL and playback_FR.

3. Run pi_setup.sh with those two values, e.g.:

	sudo ./build.sh --headless-boot \
	  --audio-left  "<FL port from diagnose>" \
	  --audio-right "<FR port from diagnose>"
EOF
