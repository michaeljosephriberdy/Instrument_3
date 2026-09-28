#include "engine.h"
#include "logger.h"

#include <iostream>
#include <exception>
#include <cstdlib>
#include <string>
#include <unistd.h>

int main()
{
    // [fix_all] If started as root (sudo), wpctl/pactl/pw-link/pw-jack only reach the desktop
    // user's PipeWire session when XDG_RUNTIME_DIR points at it. Without this, master volume
    // and routing silently do nothing under sudo.
    if (geteuid() == 0)
    {
        const char* sudo_uid = std::getenv("SUDO_UID");
        const char* xdg = std::getenv("XDG_RUNTIME_DIR");
        if (sudo_uid && *sudo_uid && (!xdg || !*xdg || std::string(xdg) == "/run/user/0"))
            setenv("XDG_RUNTIME_DIR", (std::string("/run/user/") + sudo_uid).c_str(), 1);
    }
    Logger::initialize("instrument.log");

    int result = 0;

    try
    {
        Engine engine;

        if (!engine.initialize())
        {
            std::cerr << "Engine initialization failed.\n";
            result = 1;
        }
        else
        {
            engine.run();
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Fatal error: " << e.what() << "\n";
        result = 1;
    }
    catch (...)
    {
        std::cerr << "Unknown fatal error.\n";
        result = 1;
    }

    Logger::shutdown();

    return result;
}
