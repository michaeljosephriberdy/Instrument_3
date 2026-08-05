#include "keyboard_manager.h"
#include "logger.h"

#include <poll.h>

namespace
{
    constexpr int POLL_TIMEOUT_MS = 10;
}

// Non-blocking: one pass looking for the first keypress on any
// not-yet-assigned board. Called in a spin loop by assign().
//
// NOTE: this file used to also define KeyboardManager::assignKeyboard()
// with different (incompatible) logic than the one in keyboard_manager.cpp.
// That was a duplicate-definition bug -- see Phase 0 notes. The single
// canonical assignKeyboard() now lives in keyboard_manager.cpp.
void KeyboardManager::poll()
{
    if (allAssigned())
        return;

    std::vector<pollfd> pollfds;
    pollfds.reserve(keyboards_.size());

    for (const auto& kb : keyboards_)
    {
        pollfd pfd{};
        pfd.fd = kb.fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

    if (::poll(pollfds.data(), pollfds.size(), POLL_TIMEOUT_MS) <= 0)
        return;

    for (std::size_t i = 0; i < pollfds.size(); ++i)
    {
        if (!(pollfds[i].revents & POLLIN))
            continue;

        input_event event;
        if (!readEvent(static_cast<int>(i), event))
            continue;

        if (keyboards_[i].assigned)
            continue;

        if (event.type != EV_KEY || event.value != 1)
            continue;

        assignKeyboard(static_cast<int>(i));
    }
}
