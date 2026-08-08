#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <linux/input.h>

#include "keyboard.h"

// One raw press/release event from a keyboard, already resolved to a
// board index, produced by KeyboardManager::pollPerformance().
struct KeyEvent
{
    int keyboard_index;
    int keycode;
    bool pressed;
};

class KeyboardManager
{
public:

    enum class KeyboardPosition
    {
        BottomRight,
        TopRight,
        BottomLeft,
        TopLeft
    };

    // Fired synchronously from inside assignKeyboard() the moment a
    // board is successfully assigned. Used by StartupManager (Phase 2)
    // to trigger the per-board RGB "registered" flash without
    // KeyboardManager needing to know anything about VialController.
    using AssignmentCallback =
        std::function<void(int physical_index, KeyboardPosition position)>;

    KeyboardManager();
    ~KeyboardManager();

    bool initialize();
    void shutdown();

    bool discover();

    // Blocks (spin-polling) until all 4 boards have been tapped, in
    // BottomRight -> TopRight -> BottomLeft -> TopLeft order.
    bool assign();

    bool assignKeyboard(int physical_index);
    bool allAssigned() const;

    KeyboardPosition nextAssignmentPosition() const;

    void setAssignmentCallback(AssignmentCallback callback);

    // Non-blocking. Call once per assign() spin iteration.
    void poll();

    // Non-blocking. Call once per Engine::run() tick once all boards are
    // assigned; appends every pending press/release across all boards.
    void pollPerformance(std::vector<KeyEvent>& out);

    std::vector<Keyboard>& keyboards();
    const std::vector<Keyboard>& keyboards() const;

    const std::map<int, Key>& keymap() const;

private:

    bool discoverKeyboards();
    bool buildKeymap();
    bool readEvent(int index, input_event& event);
    void applyTuning(Keyboard& kb, int logical_index);

private:

    std::vector<Keyboard> keyboards_;
    std::map<int, Key> keymap_;

    int next_assignment_index_ = 0;

    AssignmentCallback assignment_callback_;

    std::vector<KeyboardPosition> assignment_order_
    {
        KeyboardPosition::BottomRight,
        KeyboardPosition::TopRight,
        KeyboardPosition::BottomLeft,
        KeyboardPosition::TopLeft
    };
};
