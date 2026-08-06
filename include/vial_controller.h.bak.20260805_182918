#pragma once

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
    static constexpr uint16_t USAGE_ID   = 0x61;


    static constexpr uint8_t REPORT_LENGTH = 32;


    static constexpr uint8_t COMMAND_LIGHTING_SET = 0x07;
    static constexpr uint8_t COMMAND_LIGHTING_SAVE = 0x09;


    static constexpr uint8_t COMMAND_KEYMAP_SET = 0x05;


    static constexpr uint8_t VIALRGB_MODE = 0x41;
};

// NOTE (Phase 2): this header used to also declare getKeycode()/
// setKeycode() here, OUTSIDE the class body, implemented in
// vial_controller.cpp against a nonexistent `device_` member. That
// was dead, non-compiling code (Phase 0 bug list, item 5) and is
// unrelated to Phase 2's RGB scope, so it's been removed rather than
// fixed. Dynamic keymap programming via VIA/Vial (VialProtocol) is a
// Phase 3 concern if/when it's actually needed -- our note mapping
// happens entirely through evdev polling (KeyboardManager), not
// firmware-side keymap changes.
