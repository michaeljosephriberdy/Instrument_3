#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <hidapi/hidapi.h>
class VialController
{
public:
 struct Color
 {
 uint8_t hue;
 uint8_t saturation;
 uint8_t brightness;
 };
 VialController();
 ~VialController();
 bool initialize();
 void shutdown();
 int deviceCount() const;
 // Path of the underlying hidraw device node (e.g. "/dev/hidraw3"),
 // used by usbDevicePortAddress() to correlate a Vial RGB device
 // with the KeyboardManager keyboard it physically belongs to.
 // Returns an empty string for an out-of-range index.
 const std::string& devicePath(int device_index) const;
 bool setColor(
 int device_index,
 const Color& color
 );
 bool turnOff(
 int device_index
 );
 bool flash(
 int device_index,
 const Color& color,
 int milliseconds
 );
 bool save(
 int device_index
 );
 bool configureKeyboardLayout(
 int device_index,
 uint8_t layer,
 uint8_t row,
 uint8_t column,
 uint16_t keycode
 );
 // Reads back a single keycode from the board's dynamic keymap.
 // Command 0x04, response bytes 4-5 = keycode, big-endian.
 bool getKeycode(
 int device_index,
 uint8_t layer,
 uint8_t row,
 uint8_t column,
 uint16_t& keycode
 );
 // Programs every (row, col) on layer 0 from `keycodes` (row-major,
 // ID75_FIRMWARE_ROWS x ID75_FIRMWARE_COLS -- see
 // include/id75_firmware_keymap.h) via configureKeyboardLayout(), then
 // reads each one back and confirms it matches. Returns false on the
 // first mismatch or communication failure, leaving whatever was
 // written up to that point in place.
 bool programAndVerifyLayout(
 int device_index,
 const std::array<uint16_t, 75>& keycodes
 );
private:
 struct Device
 {
 hid_device* handle;
 std::string path;
 bool connected;
 };
 std::vector<Device> devices_;
private:
 bool discover();
 bool openDevice(
 const std::string& path
 );
 bool sendCommand(
 hid_device* device,
 const std::vector<uint8_t>& payload,
 std::vector<uint8_t>& response
 );
 bool setLightingMode(
 hid_device* device,
 uint8_t mode,
 const Color& color
 );
private:
 static constexpr uint16_t TARGET_VID = 0x6964;
 static constexpr uint16_t TARGET_PID = 0x0075;
 static constexpr uint16_t USAGE_PAGE = 0xFF60;
 static constexpr uint16_t USAGE_ID = 0x61;
 static constexpr uint8_t REPORT_LENGTH = 32;
 static constexpr uint8_t COMMAND_LIGHTING_SET = 0x07;
 static constexpr uint8_t COMMAND_LIGHTING_SAVE = 0x09;
 static constexpr uint8_t COMMAND_KEYMAP_GET = 0x04;
 static constexpr uint8_t COMMAND_KEYMAP_SET = 0x05;
 static constexpr uint8_t VIALRGB_MODE = 0x41;
};
// UPDATED: getKeycode()/programAndVerifyLayout() were previously removed
// from this header (see project history) because the original
// implementation referenced a nonexistent `device_` member and was dead,
// non-compiling code unrelated to the RGB-only scope of that phase. This
// reintroduction is deliberate, not a repeat of that bug: both methods now
// go through the same `devices_` vector and sendCommand()/hid_device*
// pattern every other method here already uses, and are only called from
// StartupManager's boot ceremony to enforce that each board's firmware
// keymap agrees with what KeyboardManager's evdev-side ID75_LAYOUT
// (include/keyboard_layout.h) expects at each row/col. Dynamic keymap
// writes are immediate on this firmware -- no separate save step, unlike
// lighting.
