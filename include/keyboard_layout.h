#pragma once

#include <array>
#include <cstdint>
#include <linux/input-event-codes.h>

constexpr int ID75_ROWS = 5;
constexpr int ID75_COLS = 15;
constexpr int ID75_KEYS = ID75_ROWS * ID75_COLS;

struct PhysicalKey {
    int linux_keycode;
    uint8_t row;
    uint8_t col;
};

using PhysicalLayout = std::array<PhysicalKey, ID75_KEYS>;

constexpr PhysicalLayout ID75_LAYOUT = {{

{KEY_F12,0,0},
{KEY_F13,0,1},
{KEY_DOWN,0,2},
{KEY_F15,0,3},
{KEY_F16,0,4},
{KEY_F17,0,5},
{KEY_F18,0,6},
{KEY_F19,0,7},
{KEY_F20,0,8},
{KEY_F21,0,9},
{KEY_F22,0,10},
{KEY_F23,0,11},
{KEY_F24,0,12},
{KEY_LEFT,0,13},
{KEY_RIGHT,0,14},

{KEY_BACKSLASH,1,0},
{KEY_UP,1,1},
{KEY_ENTER,1,2},
{KEY_ESC,1,3},
{KEY_F1,1,4},
{KEY_F2,1,5},
{KEY_F3,1,6},
{KEY_F4,1,7},
{KEY_F5,1,8},
{KEY_F6,1,9},
{KEY_F7,1,10},
{KEY_F8,1,11},
{KEY_F9,1,12},
{KEY_F10,1,13},
{KEY_F11,1,14},

{KEY_5,2,0},
{KEY_6,2,1},
{KEY_7,2,2},
{KEY_8,2,3},
{KEY_9,2,4},
{KEY_0,2,5},
{KEY_BACKSPACE,2,6},
{KEY_LEFTBRACE,2,7},
{KEY_RIGHTBRACE,2,8},
{KEY_SEMICOLON,2,9},
{KEY_APOSTROPHE,2,10},
{KEY_GRAVE,2,11},
{KEY_COMMA,2,12},
{KEY_DOT,2,13},
{KEY_SLASH,2,14},

{KEY_P,3,0},
{KEY_Q,3,1},
{KEY_R,3,2},
{KEY_S,3,3},
{KEY_T,3,4},
{KEY_U,3,5},
{KEY_V,3,6},
{KEY_W,3,7},
{KEY_X,3,8},
{KEY_Y,3,9},
{KEY_Z,3,10},
{KEY_1,3,11},
{KEY_2,3,12},
{KEY_3,3,13},
{KEY_4,3,14},

{KEY_A,4,0},
{KEY_B,4,1},
{KEY_C,4,2},
{KEY_D,4,3},
{KEY_E,4,4},
{KEY_F,4,5},
{KEY_G,4,6},
{KEY_H,4,7},
{KEY_I,4,8},
{KEY_J,4,9},
{KEY_K,4,10},
{KEY_L,4,11},
{KEY_M,4,12},
{KEY_N,4,13},
{KEY_O,4,14}

}};

enum class KeyboardOrientation
{
    Normal,
    Rotated180
};
