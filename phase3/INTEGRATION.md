# Phase 3 Integration Guide

## What this package contains

```
phase3/
  include/
    actions.h              # expanded ActionType + string conversion
    layout_manager.h       # JSON layouts → Action grid
    config_manager.h       # instrument.json loader
    instrument_state.h     # mixer levels, panic, mode, transpose helpers
    engine.h               # Phase-3 Engine declaration
  src/
    layout_manager.cpp     # Physical Layout V1 builder + JSON I/O
    config_manager.cpp
    instrument_state.cpp
    engine_phase3_handlers.cpp   # handleKeyEvent / handleAction / centsToMidi
  config/
    instrument.json
    layouts/
      bottom_left.json     # 75 note keys, absolute cents
      bottom_right.json
      top_left.json        # modes, mixer, drums, loop, system
      top_right.json
  CMakeLists.txt.snippet
  INTEGRATION.md           # this file
```

## Integration steps (on your Ubuntu tree)

### 1. Copy headers and sources

```bash
cp phase3/include/actions.h            include/
cp phase3/include/layout_manager.h     include/
cp phase3/include/config_manager.h     include/
cp phase3/include/instrument_state.h   include/
cp phase3/include/engine.h             include/

cp phase3/src/layout_manager.cpp       src/
cp phase3/src/config_manager.cpp       src/
cp phase3/src/instrument_state.cpp     src/
```

### 2. Copy default config

```bash
mkdir -p config/layouts
cp phase3/config/instrument.json       config/
cp phase3/config/layouts/*.json        config/layouts/
```

### 3. CMake

Apply the changes in `CMakeLists.txt.snippet`:

- `FetchContent` for nlohmann/json v3.11.3
- Add `src/config_manager.cpp` and `src/layout_manager.cpp` to the executable
- Link `nlohmann_json::nlohmann_json`

### 4. Wire Engine

In `src/engine.cpp`:

**a. Includes / members** (match `include/engine.h`):

```cpp
#include "config_manager.h"
#include "layout_manager.h"
#include "actions.h"
```

Add members:

```cpp
std::unique_ptr<ConfigManager> config_;
std::unique_ptr<LayoutManager> layouts_;
int mix_step_ = 5;
int transpose_step_ = 1;
int drum_channel_ = 9;
```

**b. In `initialize()`, after StartupManager succeeds:**

```cpp
config_ = std::make_unique<ConfigManager>();
config_->load("config/instrument.json");

const auto& cfg = config_->configuration();
mix_step_ = cfg.controls.mix_step;
transpose_step_ = cfg.controls.transpose_step;
drum_channel_ = cfg.audio.drum_channel;
state_->setTranspose(cfg.audio.default_transpose);
state_->setMasterVolume(cfg.audio.default_volume);

layouts_ = std::make_unique<LayoutManager>();
layouts_->loadAll(cfg.layouts_dir);
```

Also use `cfg.audio.zyn_instrument` for the Zyn path if you want it configurable.

**c. Replace `handleKeyEvent` and add the new methods**

Copy the bodies from `src/engine_phase3_handlers.cpp` into `engine.cpp`
(or `#include` a detail file if you prefer). Remove the old Phase-1
note-only `handleKeyEvent` that used `key.column` + `row_cents`.

**d. Optional: panic drain in `run()`**

```cpp
if (state_->consumePanic())
    allNotesOff();
```

### 5. Build

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### 6. Verify without hardware

```bash
INSTRUMENT_DESKTOP=1 ./microtonal_instrument
```

You should see log lines that layouts loaded. Editing
`config/layouts/bottom_right.json` (change a `"cents"` value) and
restarting should change that key’s pitch — no rebuild.

With hardware: bottom boards play the microtonal grid; top boards
transpose / change mode / adjust mix (logged); percussion hits drum
channel 10; Panic silences everything.

## Physical Layout V1 mapping (reference)

### Bottom boards — absolute cents

| Board | Column direction | Row rails (top→bottom) |
|-------|------------------|-------------------------|
| Left  | high→low `(14−c)×100` | +10, +20, +30, +40, **0 (ET)** |
| Right | low→high `c×100`      | −50, −40, −30, −20, **0 (ET)** |

Engine pitch:

```
absolute_cents = octave*1200 + transpose*100 + layout_cents
midi_note      = round(absolute_cents / 100)
pitch_bend     = residual cents
```

### Top boards — command rows

| Row | Columns (left→right) |
|-----|----------------------|
| 0 Loop | Record, Play, Stop, Overdub, Undo, Clear |
| 1 Mode | Mode1 (Synth), Mode2 (Vocoder), Mode3 (Vocoder+Dry) |
| 2 Mixer | Master±, Vocoder±, Drum±, Dry± |
| 3 Perc | Bass, Snare, HHOpen, HHClosed, Ride, Crash, LTom, MTom, HTom, … |
| 4 System | Panic, Transpose±, Octave± |

Top boards are marked `inverted: true` in JSON (mounted upside-down).

## What is still Phase 4

- Real PipeWire gain for dry / vocoder / drum mix values
- Looper OSC/MIDI output
- Vocoder routing for Mode 2 / 3
- Live mixer feedback to hardware

Mixer and mode state are already stored in `InstrumentState` so Phase 4
only has to read them.
