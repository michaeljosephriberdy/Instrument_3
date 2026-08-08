#include "keyboard_manager.h"
#include "logger.h"

#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

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
    // channel_offset still splits left/right onto independent MIDI channel
    // banks so each physical row can hold its own pitch-bend state.
    // Microtonal offsets themselves come solely from layout JSON cents
    // (see Engine Note path -- bend = residual). Zero the old Instrument_2
    // per-row shims so they cannot fight the layout.
    bool group_a = (logical_index == 0 || logical_index == 1);
    kb.channel_offset = group_a ? 0 : 5;
    for (int r = 0; r < ID75_ROWS; ++r)
        kb.row_cents[r] = 0;
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

int KeyboardManager::liveCount() const
{
 int n = 0;
 for (const auto& kb : keyboards_)
 if (kb.connected && kb.fd >= 0)
 ++n;
 return n;
}

void KeyboardManager::resetAssignments()
{
 next_assignment_index_ = 0;
 for (auto& kb : keyboards_) {
 kb.assigned = false;
 kb.logical_index = -1;
 }
 Logger::info("Keyboard assignments cleared — waiting for re-tap BR → TR → BL → TL");
}

int KeyboardManager::checkLiveness()
{
 // 1) Drop boards whose event node is gone or whose fd is in error.
 for (auto& kb : keyboards_) {
 if (kb.fd < 0) {
 kb.connected = false;
 continue;
 }
 // Path gone?
 if (!kb.event_path.empty() && access(kb.event_path.c_str(), F_OK) != 0) {
 Logger::warning("Keyboard node vanished: " + kb.event_path);
 ioctl(kb.fd, EVIOCGRAB, 0);
 close(kb.fd);
 kb.fd = -1;
 kb.connected = false;
 kb.assigned = false;
 continue;
 }
 // fd error/hangup?
 struct pollfd pfd{};
 pfd.fd = kb.fd;
 pfd.events = POLLIN | POLLERR | POLLHUP;
 if (::poll(&pfd, 1, 0) > 0) {
 if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
 Logger::warning("Keyboard fd error/hangup: " + kb.event_path);
 ioctl(kb.fd, EVIOCGRAB, 0);
 close(kb.fd);
 kb.fd = -1;
 kb.connected = false;
 kb.assigned = false;
 }
 }
 }

 // 2) If fewer than 4 live, scan /dev/input for new ID75 event nodes.
 if (liveCount() < 4) {
 DIR* dir = opendir("/dev/input");
 if (dir) {
 struct dirent* ent;
 while ((ent = readdir(dir)) != nullptr) {
 if (strncmp(ent->d_name, "event", 5) != 0)
 continue;
 std::string path = std::string("/dev/input/") + ent->d_name;
 // Skip if already open
 bool already = false;
 for (const auto& kb : keyboards_) {
 if (kb.event_path == path && kb.fd >= 0) {
 already = true;
 break;
 }
 }
 if (already)
 continue;
 // Try open + check name for ID75 / 0x6964 vendor via EVIOCGID or name
 int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
 if (fd < 0)
 continue;
 char name[256] = {};
 if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
 close(fd);
 continue;
 }
 std::string nm(name);
 // Match same heuristic as discoverKeyboards (ID75 / vial / 0x6964 boards)
 // Exact name used by discoverKeyboards() in keyboard_discovery.cpp
 if (nm != "THH ID75 Rev2 Keyboard") {
 close(fd);
 continue;
 }
 // Grab and register
 ioctl(fd, EVIOCGRAB, 1);
 Keyboard kb;
 kb.fd = fd;
 kb.event_path = path;
 kb.connected = true;
 kb.assigned = false;
 kb.logical_index = -1;
 keyboards_.push_back(kb);
 Logger::info("Hotplug: found keyboard at " + path + " (\"" + nm + "\")");
 }
 closedir(dir);
 }
 // Prune dead slots (fd < 0) to keep vector tidy — but keep assigned ones
 // only if still live. Compact:
 std::vector<Keyboard> live;
 live.reserve(keyboards_.size());
 for (auto& kb : keyboards_) {
 if (kb.fd >= 0 && kb.connected)
 live.push_back(kb);
 else if (kb.fd >= 0) {
 ioctl(kb.fd, EVIOCGRAB, 0);
 close(kb.fd);
 }
 }
 keyboards_.swap(live);
 }

 return liveCount();
}


