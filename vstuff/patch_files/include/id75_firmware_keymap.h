#pragma once
#include <array>
#include <cstdint>

// Real ID75 firmware keymap (layer 0), extracted from the Vial export
// Rdef.vil. Row-major, 5 rows x 15 cols, in the exact (layer, row, col)
// indexing VialController::configureKeyboardLayout()/getKeycode() use --
// this is the raw HID/QMK "basic keycode" value per physical key position
// as programmed into the board's firmware.
//
// This is intentionally a different table from include/keyboard_layout.h's
// ID75_LAYOUT: that one maps physical keys -> Linux evdev keycodes for
// software-side note mapping (KeyboardManager/evdev polling). This one is
// what StartupManager writes INTO the keyboard's firmware at boot so that,
// when a physical key is pressed, the board actually reports the evdev
// keycode software expects at that row/col. They describe the same
// physical layout from two different layers (firmware vs. OS), and must
// stay in agreement -- that agreement is exactly what
// VialController::programAndVerifyLayout() enforces at every startup.
//
// Values are standard USB HID keyboard usage IDs (identical to QMK's basic
// keycode range), not QMK "quantum" keycodes, so they're stable across QMK
// versions and don't depend on this firmware's build.
constexpr int ID75_FIRMWARE_ROWS = 5;
constexpr int ID75_FIRMWARE_COLS = 15;

constexpr std::array<uint16_t, 75> ID75_FIRMWARE_KEYMAP = {
    // row 0: KC_A, KC_B, KC_C, KC_D, KC_E, KC_F, KC_G, KC_H, KC_I, KC_J, KC_K, KC_L, KC_M, KC_N, KC_O
    0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F, 0x0010, 0x0011, 0x0012,
    // row 1: KC_P, KC_Q, KC_R, KC_S, KC_T, KC_U, KC_V, KC_W, KC_X, KC_Y, KC_Z, KC_1, KC_2, KC_3, KC_4
    0x0013, 0x0014, 0x0015, 0x0016, 0x0017, 0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F, 0x0020, 0x0021,
    // row 2: KC_5, KC_6, KC_7, KC_8, KC_9, KC_0, KC_BSPACE, KC_LBRACKET, KC_RBRACKET, KC_SCOLON, KC_QUOTE, KC_GRAVE, KC_COMMA, KC_DOT, KC_SLASH
    0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x002A, 0x002F, 0x0030, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038,
    // row 3: KC_NONUS_HASH, KC_UP, KC_ENTER, KC_ESCAPE, KC_F1, KC_F2, KC_F3, KC_F4, KC_F5, KC_F6, KC_F7, KC_F8, KC_F9, KC_F10, KC_F11
    0x0032, 0x0052, 0x0028, 0x0029, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043, 0x0044,
    // row 4: KC_F12, KC_F13, KC_DOWN, KC_F15, KC_F16, KC_F17, KC_F18, KC_F19, KC_F20, KC_F21, KC_F22, KC_F23, KC_F24, KC_LEFT, KC_RIGHT
    0x0045, 0x0068, 0x0051, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073, 0x0050, 0x004F,
};
