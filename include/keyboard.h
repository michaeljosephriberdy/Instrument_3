#pragma once

#include <string>

#include "keyboard_layout.h"

// One physical key, resolved from a Linux input keycode via ID75_LAYOUT.
struct Key
{
    int linux_keycode;
    int row;
    int column;
};

// One physical ID75 keyboard, as tracked by KeyboardManager.
struct Keyboard
{
    int fd = -1;
    std::string event_path;

    bool connected = false;
    bool assigned = false;

    // Assignment order: 0=BottomRight, 1=TopRight, 2=BottomLeft, 3=TopLeft.
    // See KeyboardManager::KeyboardPosition.
    int logical_index = -1;

    // True for boards mounted upside-down (TopRight/TopLeft). Recorded but
    // NOT yet applied to note lookup -- see engine.cpp handleKeyEvent().
    bool inverted = false;

    // MIDI channel base for this keyboard (0 or 5), set by applyTuning().
    int channel_offset = 0;

    // Pitch-bend offset in cents, one entry per physical row, set by
    // applyTuning(). Phase-1 values are ported directly from Instrument_2.
    int row_cents[ID75_ROWS] = {0, 0, 0, 0, 0};

    // Index into VialController's device list for this board's RGB,
    // resolved by StartupManager via usbDevicePortAddress() after
    // discovery. -1 until correlated (or if correlation failed/no
    // Vial-capable RGB was found for this board).
    int rgb_device_index = -1;
};
