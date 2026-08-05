#include "keyboard_manager.h"
#include "logger.h"

#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>

namespace
{
    // Phase 1 tuning: ported directly from Instrument_2's
    // configure_keyboard(). The first two boards assigned (in
    // BottomRight -> TopRight -> BottomLeft -> TopLeft order) get the
    // "positive" group, the last two get "negative". These are small
    // microtonal pitch-bend shims per row, in cents -- NOT the wider
    // per-row semitone spread described in the design doc's Section 6,
    // which is deliberately deferred to Phase 3 (it changes musical
    // behavior and needs to be tuned/verified on the instrument, not
    // guessed at from a spec while the mechanics are still being proven).
    constexpr int GROUP_A_CENTS[ID75_ROWS] = {0, 10, 20, 30, 40};
    constexpr int GROUP_B_CENTS[ID75_ROWS] = {-50, -40, -30, -20, -10};

    constexpr const char* POSITION_NAMES[4] =
    {
        "Bottom Right",
        "Top Right",
        "Bottom Left",
        "Top Left"
    };
}

KeyboardManager::KeyboardManager()
{
}

KeyboardManager::~KeyboardManager()
{
    shutdown();
}

bool KeyboardManager::initialize()
{
    Logger::info("Initializing KeyboardManager...");

    keyboards_.clear();
    keymap_.clear();
    next_assignment_index_ = 0;

    // CHANGED (Phase 2): this used to also call discover() and hard-fail
    // if fewer than 4 boards were present. Discovery-with-retry is now
    // owned by StartupManager::discoverBoards(), so it can drive RGB
    // feedback and keep retrying instead of failing immediately. This
    // method now only builds the keymap table.
    return buildKeymap();
}

void KeyboardManager::shutdown()
{
    for (auto& kb : keyboards_)
    {
        if (kb.fd >= 0)
        {
            ioctl(kb.fd, EVIOCGRAB, 0);
            close(kb.fd);
            kb.fd = -1;
        }
    }

    keyboards_.clear();
}

bool KeyboardManager::discover()
{
    // FIX (Phase 2): discover() can now be called repeatedly (by
    // StartupManager's retry loop), and keyboards_.clear() alone would
    // leak the previous attempt's open fds. Close them first.
    for (auto& kb : keyboards_)
    {
        if (kb.fd >= 0)
        {
            ioctl(kb.fd, EVIOCGRAB, 0);
            close(kb.fd);
            kb.fd = -1;
        }
    }

    keyboards_.clear();
    return discoverKeyboards();
}

bool KeyboardManager::assign()
{
    Logger::info(
        "Waiting for keyboard assignment: touch any key on Bottom Right, "
        "then Top Right, then Bottom Left, then Top Left."
    );

    while (!allAssigned())
    {
        poll();
    }

    Logger::info("All keyboards assigned.");
    return true;
}

bool KeyboardManager::assignKeyboard(int physical)
{
    if (physical < 0 || physical >= static_cast<int>(keyboards_.size()))
        return false;

    Keyboard& kb = keyboards_[physical];

    if (kb.assigned)
        return false;

    if (next_assignment_index_ >= static_cast<int>(assignment_order_.size()))
        return false;

    kb.logical_index = next_assignment_index_;

    // Top boards (logical index 1 = TopRight, 3 = TopLeft) are mounted
    // upside-down per the design doc. Recorded here for Phase 2/3 use;
    // NOT yet applied to note lookup. See engine.cpp handleKeyEvent().
    kb.inverted = (kb.logical_index == 1 || kb.logical_index == 3);

    applyTuning(kb, kb.logical_index);
    kb.assigned = true;

    Logger::info(
        std::string("Assigned physical keyboard ") +
        std::to_string(physical) +
        " -> " +
        POSITION_NAMES[kb.logical_index]
    );

    next_assignment_index_++;

    if (assignment_callback_)
    {
        assignment_callback_(physical, static_cast<KeyboardPosition>(kb.logical_index));
    }

    return true;
}

void KeyboardManager::setAssignmentCallback(AssignmentCallback callback)
{
    assignment_callback_ = std::move(callback);
}

void KeyboardManager::applyTuning(Keyboard& kb, int logical_index)
{
    bool group_a = (logical_index == 0 || logical_index == 1);

    kb.channel_offset = group_a ? 0 : 5;

    const int* src = group_a ? GROUP_A_CENTS : GROUP_B_CENTS;
    for (int r = 0; r < ID75_ROWS; ++r)
    {
        kb.row_cents[r] = src[r];
    }
}

bool KeyboardManager::allAssigned() const
{
    if (keyboards_.empty())
        return false;

    for (const auto& kb : keyboards_)
    {
        if (!kb.assigned)
            return false;
    }

    return true;
}

KeyboardManager::KeyboardPosition KeyboardManager::nextAssignmentPosition() const
{
    return assignment_order_[next_assignment_index_];
}

std::vector<Keyboard>& KeyboardManager::keyboards()
{
    return keyboards_;
}

const std::vector<Keyboard>& KeyboardManager::keyboards() const
{
    return keyboards_;
}

const std::map<int, Key>& KeyboardManager::keymap() const
{
    return keymap_;
}

bool KeyboardManager::buildKeymap()
{
    keymap_.clear();

    for (const auto& pk : ID75_LAYOUT)
    {
        keymap_[pk.linux_keycode] = Key{pk.linux_keycode, pk.row, pk.col};
    }

    return true;
}

bool KeyboardManager::readEvent(int index, input_event& event)
{
    if (index < 0 || index >= static_cast<int>(keyboards_.size()))
        return false;

    ssize_t n = ::read(keyboards_[index].fd, &event, sizeof(event));
    return n == static_cast<ssize_t>(sizeof(event));
}

void KeyboardManager::pollPerformance(std::vector<KeyEvent>& out)
{
    out.clear();

    std::vector<pollfd> pollfds;
    pollfds.reserve(keyboards_.size());

    for (const auto& kb : keyboards_)
    {
        pollfd pfd{};
        pfd.fd = kb.fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

    if (::poll(pollfds.data(), pollfds.size(), 0) <= 0)
        return;

    for (std::size_t i = 0; i < pollfds.size(); ++i)
    {
        if (!(pollfds[i].revents & POLLIN))
            continue;

        input_event event;
        if (!readEvent(static_cast<int>(i), event))
            continue;

        if (event.type != EV_KEY || event.value == 2) // ignore autorepeat
            continue;

        out.push_back(KeyEvent{static_cast<int>(i), event.code, event.value == 1});
    }
}
