#pragma once

#include "vial_controller.h"

class KeyboardManager;
class BreathController;

// Owns the appliance-style startup sequence from the design doc,
// Section 3: discover boards (retrying with RGB feedback instead of
// failing immediately), tap-to-assign with per-board RGB, then
// non-fatal breath/microphone checks, then idle.
//
// CHANGED (Phase 2): this used to take DeviceManager&, LayoutManager&,
// and RGBManager& as well. DeviceManager was retired in Phase 0
// (redundant with KeyboardManager/BreathController). LayoutManager
// (JSON layouts) is Phase 3 -- not needed to run the startup ceremony.
// RGBManager was retired here too: it was a second, independent HID
// device-management system duplicating VialController, so this talks
// to VialController directly instead.
class StartupManager
{
public:

    StartupManager(
        KeyboardManager& keyboard_manager,
        VialController& vial_controller,
        BreathController& breath_controller
    );

    bool run();


private:

    bool discoverBoards();

    void correlateRgbDevices();

    bool assignAndProgramBoards();

    bool verifyBreathController();

    bool verifyMicrophone();

    bool finalHealthCheck();


private:

    void setAllVialDevices(const VialController::Color& color);

    void turnOffAllVialDevices();

    void flashFatalError();


private:

    KeyboardManager& keyboard_manager_;

    VialController& vial_controller_;

    BreathController& breath_controller_;
};
