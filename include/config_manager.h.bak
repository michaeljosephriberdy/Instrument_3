#pragma once

#include <string>
#include <vector>

#include "keyboard_manager.h"

class ConfigManager
{
public:
    struct KeyboardConfiguration
    {
        KeyboardManager::KeyboardPosition position;
        bool inverted = false;
        std::string layout_file;
    };

    struct AudioConfiguration
    {
        std::string zyn_instrument = "config/mkb.xmz";
        int default_volume = 127;
        int default_transpose = 0;
        int drum_channel = 9;   // MIDI channel for percussion (0-based: 9 = ch 10)
    };

    struct ControlConfiguration
    {
        int breath_volume_cc = 7;
        int nod_controller_cc = 1;
        int mix_step = 5;       // mixer ± step size (0-127 scale)
        int transpose_step = 1;
    };

    struct InstrumentConfiguration
    {
        std::vector<KeyboardConfiguration> keyboards;
        AudioConfiguration audio;
        ControlConfiguration controls;
        std::string layouts_dir = "config/layouts";
    };

    ConfigManager();

    bool load(const std::string& filename);
    bool save(const std::string& filename) const;

    const InstrumentConfiguration& configuration() const;
    bool setConfiguration(const InstrumentConfiguration& config);

    // Fills a sensible default (Physical Layout V1 file names, Zyn path).
    bool createDefaultConfiguration();

private:
    InstrumentConfiguration config_;

    std::string positionToString(KeyboardManager::KeyboardPosition position) const;
    KeyboardManager::KeyboardPosition stringToPosition(const std::string& value) const;
};
