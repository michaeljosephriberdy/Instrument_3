#include "startup_manager.h"

#include "logger.h"
#include "keyboard_manager.h"
#include "breath_controller.h"
#include "usb_topology.h"
#include "status_colors.h"

#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstdio>

namespace
{
    constexpr int DISCOVERY_MAX_ATTEMPTS = 30;
    constexpr int DISCOVERY_RETRY_DELAY_MS = 1000;

    constexpr int REGISTERED_FLASH_MS = 400;
    constexpr int BREATH_MISSING_FLASH_MS = 800;

    constexpr int FATAL_FLASH_CYCLES = 6;
    constexpr int FATAL_FLASH_MS = 250;
}

StartupManager::StartupManager(
    KeyboardManager& keyboard_manager,
    VialController& vial_controller,
    BreathController& breath_controller
)
:
keyboard_manager_(keyboard_manager),
vial_controller_(vial_controller),
breath_controller_(breath_controller)
{
}

bool StartupManager::run()
{
    Logger::info("=== Startup sequence beginning ===");

    // Boot indication: blue on everything we can currently see.
    setAllVialDevices(StatusColors::Blue);

    if (!discoverBoards())
    {
        Logger::error("Startup failed: could not find all 4 keyboards.");
        flashFatalError();
        return false;
    }

    correlateRgbDevices();

    if (!assignAndProgramBoards())
    {
        Logger::error("Startup failed during keyboard assignment.");
        flashFatalError();
        return false;
    }

    // Non-fatal checks: log + brief RGB indication, but never block
    // startup on these, matching Instrument_2's tolerance of a missing
    // breath rig.
    verifyBreathController();
    verifyMicrophone();

    finalHealthCheck();

    Logger::info("=== Startup sequence complete ===");
    return true;
}

bool StartupManager::discoverBoards()
{
    // Desktop / CI mode: short retry, then continue even with 0 boards so
    // the rest of the engine can be developed and tested on Ubuntu without
    // the four ID75s. Set INSTRUMENT_DESKTOP=1 in the environment.
    const bool desktop = (std::getenv("INSTRUMENT_DESKTOP") != nullptr);
    const int max_attempts = desktop ? 3 : DISCOVERY_MAX_ATTEMPTS;
    const int retry_ms     = desktop ? 300 : DISCOVERY_RETRY_DELAY_MS;

    for (int attempt = 1; attempt <= max_attempts; ++attempt)
    {
        keyboard_manager_.discover();

        std::size_t found = keyboard_manager_.keyboards().size();

        if (found == 4)
        {
            Logger::info("All 4 keyboards detected.");
            return true;
        }

        Logger::warning(
            "Found " + std::to_string(found) +
            "/4 keyboards (attempt " + std::to_string(attempt) +
            "/" + std::to_string(max_attempts) + "). Retrying..."
        );

        setAllVialDevices(StatusColors::Yellow);

        std::this_thread::sleep_for(
            std::chrono::milliseconds(retry_ms)
        );
    }

    if (desktop)
    {
        Logger::warning(
            "Desktop mode: proceeding with " +
            std::to_string(keyboard_manager_.keyboards().size()) +
            " keyboard(s). Assignment stage will be skipped if fewer than 4."
        );
        return true; // non-fatal under desktop mode
    }

    Logger::error(
        "Timed out after " + std::to_string(DISCOVERY_MAX_ATTEMPTS) +
        " attempts waiting for all 4 keyboards."
    );

    return false;
}

void StartupManager::correlateRgbDevices()
{
    Logger::info("Correlating keyboards to their RGB (Vial) devices...");

    for (auto& kb : keyboard_manager_.keyboards())
    {
        kb.rgb_device_index = -1;

        std::string kb_port = usbDevicePortAddress(kb.event_path);

        if (kb_port.empty())
        {
            Logger::warning(
                "Could not resolve USB port for " + kb.event_path +
                " -- RGB feedback will be unavailable for this board."
            );
            continue;
        }

        for (int i = 0; i < vial_controller_.deviceCount(); ++i)
        {
            if (usbDevicePortAddress(vial_controller_.devicePath(i)) == kb_port)
            {
                kb.rgb_device_index = i;
                break;
            }
        }

        if (kb.rgb_device_index < 0)
        {
            Logger::warning(
                "No matching Vial RGB device found for " + kb.event_path +
                " -- RGB feedback will be unavailable for this board."
            );
        }
    }
}

bool StartupManager::assignAndProgramBoards()
{
    const auto& boards = keyboard_manager_.keyboards();

    if (boards.empty())
    {
        Logger::warning("No keyboards present — skipping assignment stage.");
        return true;
    }

    if (boards.size() < 4)
    {
        // Desktop / partial-hardware: auto-assign whatever is present in
        // discovery order so the performance loop can still be exercised.
        Logger::warning(
            "Only " + std::to_string(boards.size()) +
            " keyboard(s) present. Auto-assigning in discovery order "
            "(desktop / partial-hardware mode)."
        );

        for (std::size_t i = 0; i < boards.size(); ++i)
        {
            if (!boards[i].assigned)
                keyboard_manager_.assignKeyboard(static_cast<int>(i));
        }
        return true;
    }

    Logger::info(
        "Waiting for keyboard assignment: touch any key on Bottom Right, "
        "then Top Right, then Bottom Left, then Top Left."
    );

    for (const auto& kb : boards)
    {
        if (kb.rgb_device_index >= 0)
        {
            vial_controller_.setColor(kb.rgb_device_index, StatusColors::White);
        }
    }

    keyboard_manager_.setAssignmentCallback(
        [this](int physical_index, KeyboardManager::KeyboardPosition)
        {
            const auto& kb = keyboard_manager_.keyboards()[physical_index];

            if (kb.rgb_device_index >= 0)
            {
                vial_controller_.flash(
                    kb.rgb_device_index,
                    StatusColors::Green,
                    REGISTERED_FLASH_MS
                );
            }
        }
    );

    while (!keyboard_manager_.allAssigned())
    {
        keyboard_manager_.poll();
    }

    keyboard_manager_.setAssignmentCallback(nullptr);

    Logger::info("All keyboards assigned.");
    return true;
}

bool StartupManager::verifyBreathController()
{
    Logger::info("Checking breath controller...");

    if (breath_controller_.initialize())
    {
        Logger::info("Breath controller connected.");
        return true;
    }

    Logger::warning("Breath controller not found. Continuing without it.");

    setAllVialDevices(StatusColors::Magenta);
    std::this_thread::sleep_for(std::chrono::milliseconds(BREATH_MISSING_FLASH_MS));
    turnOffAllVialDevices();

    // Non-fatal: matches Instrument_2's tolerance of a missing breath
    // rig, which is useful for bench-testing keyboards alone.
    return true;
}

bool StartupManager::verifyMicrophone()
{
    Logger::info("Checking USB microphone interface (Shure MVX2U / capture)...");

    // Best-effort: PipeWire port list or ALSA card names. Non-fatal always.
    bool present = false;
    std::string detail;

    // 1) PipeWire: any capture-related node mentioning MVX2U / Shure / USB Audio.
    {
        FILE* pipe = popen(
            "pw-cli list-objects 2>/dev/null | grep -iE "
            "'node.name|node.description' | grep -iE 'MVX2U|Shure|USB.?Audio|Microphone' || true",
            "r");
        if (pipe)
        {
            char buf[512];
            while (fgets(buf, sizeof(buf), pipe))
            {
                present = true;
                detail = buf;
                while (!detail.empty()
                       && (detail.back() == '\n' || detail.back() == '\r'))
                {
                    detail.pop_back();
                }
                break;
            }
            pclose(pipe);
        }
    }

    // 2) ALSA fallback: cards
    if (!present)
    {
        FILE* pipe = popen(
            "cat /proc/asound/cards 2>/dev/null | grep -iE 'MVX2U|Shure|USB' || true",
            "r");
        if (pipe)
        {
            char buf[512];
            while (fgets(buf, sizeof(buf), pipe))
            {
                present = true;
                detail = buf;
                while (!detail.empty()
                       && (detail.back() == '\n' || detail.back() == '\r'))
                {
                    detail.pop_back();
                }
                break;
            }
            pclose(pipe);
        }
    }

    if (present)
    {
        Logger::info("Microphone interface detected"
                     + (detail.empty() ? std::string("") : (": " + detail)));
        return true;
    }

    Logger::warning("Microphone interface not found. Continuing without it "
                    "(Mode 2/3 will degrade until mic is present).");
    setAllVialDevices(StatusColors::Cyan);
    std::this_thread::sleep_for(std::chrono::milliseconds(BREATH_MISSING_FLASH_MS));
    turnOffAllVialDevices();
    return true; // non-fatal
}


bool StartupManager::finalHealthCheck()
{
    Logger::info("Startup complete. Clearing RGB to idle.");

    turnOffAllVialDevices();

    return true;
}

void StartupManager::setAllVialDevices(const VialController::Color& color)
{
    for (int i = 0; i < vial_controller_.deviceCount(); ++i)
    {
        vial_controller_.setColor(i, color);
    }
}

void StartupManager::turnOffAllVialDevices()
{
    for (int i = 0; i < vial_controller_.deviceCount(); ++i)
    {
        vial_controller_.turnOff(i);
    }
}

void StartupManager::flashFatalError()
{
    for (int cycle = 0; cycle < FATAL_FLASH_CYCLES; ++cycle)
    {
        setAllVialDevices(StatusColors::Red);
        std::this_thread::sleep_for(std::chrono::milliseconds(FATAL_FLASH_MS));

        turnOffAllVialDevices();
        std::this_thread::sleep_for(std::chrono::milliseconds(FATAL_FLASH_MS));
    }
}
