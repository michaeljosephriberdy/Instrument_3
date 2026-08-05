#include "keyboard_manager.h"
#include "logger.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace
{
    constexpr int MAX_EVENT_DEVICES = 32;
    constexpr const char* KEYBOARD_NAME = "THH ID75 Rev2 Keyboard";
}

// Ported directly from Instrument_2's find_keyboards(): scan
// /dev/input/eventN for boards reporting this exact device name, and
// grab each one so keystrokes don't also reach the console/X11.
bool KeyboardManager::discoverKeyboards()
{
    Logger::info("Searching for ID75 keyboards...");

    for (int i = 0; i < MAX_EVENT_DEVICES; ++i)
    {
        std::string path = "/dev/input/event" + std::to_string(i);

        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        char name[256] = {};
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        if (std::string(name) != KEYBOARD_NAME)
        {
            close(fd);
            continue;
        }

        ioctl(fd, EVIOCGRAB, 1);

        Keyboard kb;
        kb.fd = fd;
        kb.event_path = path;
        kb.connected = true;

        keyboards_.push_back(kb);

        Logger::info("Found keyboard at " + path);
    }

    Logger::info("Detected " + std::to_string(keyboards_.size()) + " keyboard(s).");

    return true;
}
