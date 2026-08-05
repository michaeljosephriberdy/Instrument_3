# Phase 4 host packages (Ubuntu)

Install when ready (not required for Mode 1 scaffold):

```bash
sudo apt update
sudo apt install -y \
  zynaddsubfx \
  sooperlooper \
  calf-plugins \
  jalv \
  liblo-tools \
  pipewire-jack \
  pipewire-audio-client-libraries
```

- **SooperLooper**: OSC on port 9951 by default (`oscsend` from liblo-tools).
- **Calf Vocoder**: LV2 URI `http://calf.sourceforge.net/plugins/Vocoder`, hosted under `jalv` or `jalv.gtk`.
- **Shure MVX2U**: appears as ALSA `card X: MVX2U` and as PipeWire ports containing `MVX2U`.

Pi: same package names on Raspberry Pi OS Bookworm with PipeWire; prefer
`pipewire-jack` so JACK clients (Zyn, SL, jalv) see the PipeWire graph via
`pw-jack`.

## Phase 4.4 notes

Calf Vocoder is hosted as:

```text
pw-jack jalv -n InstrumentVocoder http://calf.sourceforge.net/plugins/Vocoder
```

Confirm URI:

```bash
lv2ls | grep -i vocoder
```

Mixer:
- Master → `wpctl set-volume @DEFAULT_AUDIO_SINK@ <0..1>`
- Vocoder / dry → best-effort by node name (`InstrumentVocoder`, `MVX2U`)
- Drums remain MIDI channel levels for now

## Phase 4.4 notes

Calf Vocoder is hosted as:

```text
pw-jack jalv -n InstrumentVocoder http://calf.sourceforge.net/plugins/Vocoder
```

Confirm URI:

```bash
lv2ls | grep -i vocoder
```

Mixer:
- Master → `wpctl set-volume @DEFAULT_AUDIO_SINK@ <0..1>`
- Vocoder / dry → best-effort by node name (`InstrumentVocoder`, `MVX2U`)
- Drums remain MIDI channel levels for now
