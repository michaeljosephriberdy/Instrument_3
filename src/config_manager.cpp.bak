#include "config_manager.h"

#include "logger.h"

#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

ConfigManager::ConfigManager()
{
    createDefaultConfiguration();
}

bool ConfigManager::createDefaultConfiguration()
{
    config_ = InstrumentConfiguration{};

    config_.audio.zyn_instrument = "config/mkb.xmz";
    config_.audio.default_volume = 127;
    config_.audio.default_transpose = 0;
    config_.audio.drum_channel = 9;

    config_.controls.breath_volume_cc = 7;
    config_.controls.nod_controller_cc = 1;
    config_.controls.mix_step = 5;
    config_.controls.transpose_step = 1;

    config_.layouts_dir = "config/layouts";

    const struct {
        KeyboardManager::KeyboardPosition pos;
        bool inverted;
        const char* file;
    } boards[] = {
        { KeyboardManager::KeyboardPosition::BottomRight, false, "bottom_right.json" },
        { KeyboardManager::KeyboardPosition::TopRight,    true,  "top_right.json" },
        { KeyboardManager::KeyboardPosition::BottomLeft,  false, "bottom_left.json" },
        { KeyboardManager::KeyboardPosition::TopLeft,     true,  "top_left.json" },
    };

    for (const auto& b : boards)
    {
        KeyboardConfiguration kc;
        kc.position = b.pos;
        kc.inverted = b.inverted;
        kc.layout_file = std::string(b.file);
        config_.keyboards.push_back(kc);
    }

    return true;
}

bool ConfigManager::load(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open())
    {
        Logger::info(
            "Configuration file missing (" + filename + "). Using defaults."
        );
        return createDefaultConfiguration();
    }

    json j;
    try
    {
        file >> j;
    }
    catch (const json::exception& e)
    {
        Logger::error(std::string("Config JSON parse error: ") + e.what());
        return createDefaultConfiguration();
    }

    createDefaultConfiguration(); // start from defaults, overlay file

    if (j.contains("audio"))
    {
        const auto& a = j["audio"];
        config_.audio.zyn_instrument =
            a.value("zyn_instrument", config_.audio.zyn_instrument);
        config_.audio.default_volume =
            a.value("default_volume", config_.audio.default_volume);
        config_.audio.default_transpose =
            a.value("default_transpose", config_.audio.default_transpose);
        config_.audio.drum_channel =
            a.value("drum_channel", config_.audio.drum_channel);
    }

    if (j.contains("controls"))
    {
        const auto& c = j["controls"];
        config_.controls.breath_volume_cc =
            c.value("breath_volume_cc", config_.controls.breath_volume_cc);
        config_.controls.nod_controller_cc =
            c.value("nod_controller_cc", config_.controls.nod_controller_cc);
        config_.controls.mix_step =
            c.value("mix_step", config_.controls.mix_step);
        config_.controls.transpose_step =
            c.value("transpose_step", config_.controls.transpose_step);
    }

    config_.layouts_dir = j.value("layouts_dir", config_.layouts_dir);

    if (j.contains("keyboards") && j["keyboards"].is_array())
    {
        config_.keyboards.clear();
        for (const auto& k : j["keyboards"])
        {
            KeyboardConfiguration kc;
            kc.position = stringToPosition(k.value("position", std::string("bottom_right")));
            kc.inverted = k.value("inverted", false);
            kc.layout_file = k.value("layout_file", std::string(""));
            config_.keyboards.push_back(kc);
        }
    }

    Logger::info("Loaded configuration from " + filename);
    return true;
}

bool ConfigManager::save(const std::string& filename) const
{
    json j;

    j["layouts_dir"] = config_.layouts_dir;

    j["audio"] = {
        {"zyn_instrument", config_.audio.zyn_instrument},
        {"default_volume", config_.audio.default_volume},
        {"default_transpose", config_.audio.default_transpose},
        {"drum_channel", config_.audio.drum_channel}
    };

    j["controls"] = {
        {"breath_volume_cc", config_.controls.breath_volume_cc},
        {"nod_controller_cc", config_.controls.nod_controller_cc},
        {"mix_step", config_.controls.mix_step},
        {"transpose_step", config_.controls.transpose_step}
    };

    j["keyboards"] = json::array();
    for (const auto& kc : config_.keyboards)
    {
        j["keyboards"].push_back({
            {"position", positionToString(kc.position)},
            {"inverted", kc.inverted},
            {"layout_file", kc.layout_file}
        });
    }

    std::ofstream file(filename);
    if (!file.is_open())
        return false;

    file << j.dump(2) << "\n";
    return true;
}

const ConfigManager::InstrumentConfiguration&
ConfigManager::configuration() const
{
    return config_;
}

bool ConfigManager::setConfiguration(const InstrumentConfiguration& config)
{
    config_ = config;
    return true;
}

std::string ConfigManager::positionToString(
    KeyboardManager::KeyboardPosition position) const
{
    switch (position)
    {
        case KeyboardManager::KeyboardPosition::BottomRight: return "bottom_right";
        case KeyboardManager::KeyboardPosition::TopRight:    return "top_right";
        case KeyboardManager::KeyboardPosition::BottomLeft:  return "bottom_left";
        case KeyboardManager::KeyboardPosition::TopLeft:     return "top_left";
    }
    return "unknown";
}

KeyboardManager::KeyboardPosition
ConfigManager::stringToPosition(const std::string& value) const
{
    if (value == "bottom_right") return KeyboardManager::KeyboardPosition::BottomRight;
    if (value == "top_right")    return KeyboardManager::KeyboardPosition::TopRight;
    if (value == "bottom_left")  return KeyboardManager::KeyboardPosition::BottomLeft;
    if (value == "top_left")     return KeyboardManager::KeyboardPosition::TopLeft;
    return KeyboardManager::KeyboardPosition::BottomRight;
}
