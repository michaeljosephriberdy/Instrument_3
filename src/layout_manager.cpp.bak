#include <filesystem>
#include "layout_manager.h"

#include "logger.h"

#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace
{
    Action makeNote(int cents)
    {
        Action a;
        a.type = ActionType::Note;
        a.cents = cents;
        return a;
    }

    Action makeDrum(int drum_note)
    {
        Action a;
        a.type = ActionType::Drum;
        a.drum_note = drum_note;
        return a;
    }

    Action make(ActionType t)
    {
        Action a;
        a.type = t;
        return a;
    }

    // GM-ish drum map used on the top-board percussion row.
    // Columns 0..8 left-style pads; 9..14 continue / mirror.
    constexpr int DRUM_NOTES[15] = {
        36, // Bass
        38, // Snare
        46, // HH Open
        42, // HH Closed
        51, // Ride
        49, // Crash
        45, // Low Tom
        47, // Mid Tom
        50, // High Tom
        36, // Bass (repeat / right half)
        38, // Snare
        46, // HH Open
        42, // HH Closed
        51, // Ride
        49  // Crash
    };
}

LayoutManager::LayoutManager()
{
    for (auto& L : layouts_)
    {
        for (auto& a : L.actions)
            a = Action{};
    }
}

LayoutManager::BoardPosition LayoutManager::indexToPosition(int logical_index)
{
    switch (logical_index)
    {
        case 0:  return BoardPosition::BottomRight;
        case 1:  return BoardPosition::TopRight;
        case 2:  return BoardPosition::BottomLeft;
        case 3:  return BoardPosition::TopLeft;
        default: return BoardPosition::BottomRight;
    }
}

const char* LayoutManager::positionToFileStem(BoardPosition p)
{
    switch (p)
    {
        case BoardPosition::BottomRight: return "bottom_right";
        case BoardPosition::TopRight:    return "top_right";
        case BoardPosition::BottomLeft:  return "bottom_left";
        case BoardPosition::TopLeft:     return "top_left";
    }
    return "bottom_right";
}

int LayoutManager::convertColumn(int column, bool inverted)
{
    if (!inverted)
        return column;
    return (ID75_COLS - 1) - column;
}

int LayoutManager::convertRow(int row, bool inverted)
{
    if (!inverted)
        return row;
    return (ID75_ROWS - 1) - row;
}

const LayoutManager::KeyboardLayout&
LayoutManager::layout(BoardPosition position) const
{
    return layouts_[static_cast<int>(position)];
}

Action LayoutManager::actionFor(BoardPosition position, int row, int column) const
{
    if (row < 0 || row >= ID75_ROWS || column < 0 || column >= ID75_COLS)
        return Action{};

    const auto& L = layouts_[static_cast<int>(position)];
    const int r = convertRow(row, L.inverted);
    const int col = convertColumn(column, L.inverted);
    return L.actions[index(r, col)];
}

// -----------------------------------------------------------------------------
// Physical Layout V1 — cents grids from the spreadsheet
// -----------------------------------------------------------------------------
//
// Bottom boards: 5 rows × 15 cols of absolute cents from the transposed root.
//
// Left  (BottomLeft):  high → low   (14-c)*100 + rail
// Right (BottomRight): low  → high   c*100 + rail
//
// Rails (row 0 = top of bottom board → row 4 = bottom / ET root row):
//   Left:  +50, +40, +30, +20, +10, and row4 = ET (0 offset)  — sheet rows 10..11
//   Actually matching the sheet number rows (top→bottom of bottom boards):
//     sheet "row6"  ≈ physical row 0: left +10 rail relative? 
//
// Spreadsheet number rows (top to bottom of bottom section):
//   1410..10 | -50..1350     → physical row 0  (left base 10, right base -50)
//   1420..20 | -40..1360     → physical row 1
//   1430..30 | -30..1370     → physical row 2
//   1440..40 | -20..1380     → physical row 3
//   1450..50 | -10..1390     → physical row 4  (wait - sheet has 6 number rows)
//   1400..0  |  0..1400      → ET root row
//
// ID75 has 5 rows. We map the five most musically useful rails:
//   row 0: left +10 / right -50
//   row 1: left +20 / right -40
//   row 2: left +30 / right -30
//   row 3: left +40 / right -20
//   row 4: left  0  / right   0   (equal-temperament root row)
//
// That keeps the ET root on the bottom edge under the player's hands and the
// microtonal rails above it — matching "Root ↑" on the sheet.
// -----------------------------------------------------------------------------

void LayoutManager::buildPhysicalLayoutV1(KeyboardLayout& bl,
                                          KeyboardLayout& br,
                                          KeyboardLayout& tl,
                                          KeyboardLayout& tr)
{
    // ----- Bottom Left -----
    bl.name = "bottom_left";
    bl.inverted = false;
    {
        // rail offsets for rows 0..4
        const int left_rail[5] = {10, 20, 30, 40, 0};
        for (int r = 0; r < ID75_ROWS; ++r)
        {
            for (int c = 0; c < ID75_COLS; ++c)
            {
                int cents = (ID75_COLS - 1 - c) * 100 + left_rail[r];
                bl.actions[index(r, c)] = makeNote(cents);
            }
        }
    }

    // ----- Bottom Right -----
    br.name = "bottom_right";
    br.inverted = false;
    {
        const int right_rail[5] = {-50, -40, -30, -20, 0};
        for (int r = 0; r < ID75_ROWS; ++r)
        {
            for (int c = 0; c < ID75_COLS; ++c)
            {
                int cents = c * 100 + right_rail[r];
                br.actions[index(r, c)] = makeNote(cents);
            }
        }
    }

    // ----- Top Left (command surface) -----
    // Row mapping (Physical Layout V1 labels):
    //   row 0 — Loop controls
    //   row 1 — Mode 1/2/3 + extras
    //   row 2 — Mixer (master / vocoder / percussion)
    //   row 3 — Percussion pads
    //   row 4 — System (panic, transpose, empty for now)
    tl.name = "top_left";
    tl.inverted = true;  // top boards mounted upside-down
    for (auto& a : tl.actions)
        a = Action{};

    // Row 0: Loop
    {
        const ActionType loop_row[15] = {
            ActionType::LoopRecord, ActionType::LoopPlay, ActionType::LoopStop,
            ActionType::LoopOverdub, ActionType::LoopUndo, ActionType::LoopClear,
            ActionType::None, ActionType::None, ActionType::None,
            ActionType::None, ActionType::None, ActionType::None,
            ActionType::None, ActionType::None, ActionType::None
        };
        for (int c = 0; c < ID75_COLS; ++c)
            tl.actions[index(0, c)] = make(loop_row[c]);
    }

    // Row 1: Modes
    {
        const ActionType mode_row[15] = {
            ActionType::ModeInstrument, ActionType::ModeVocoder, ActionType::ModeVocoderDry,
            ActionType::None, ActionType::None, ActionType::None,
            ActionType::None, ActionType::None, ActionType::None,
            ActionType::None, ActionType::None, ActionType::None,
            ActionType::None, ActionType::None, ActionType::None
        };
        for (int c = 0; c < ID75_COLS; ++c)
            tl.actions[index(1, c)] = make(mode_row[c]);
    }

    // Row 2: Mixer
    {
        const ActionType mix_row[15] = {
            ActionType::MasterVolUp, ActionType::MasterVolDown,
            ActionType::VocoderMixUp, ActionType::VocoderMixDown,
            ActionType::DrumMixUp, ActionType::DrumMixDown,
            ActionType::DryMixUp, ActionType::DryMixDown,
            ActionType::None, ActionType::None, ActionType::None,
            ActionType::None, ActionType::None, ActionType::None, ActionType::None
        };
        for (int c = 0; c < ID75_COLS; ++c)
            tl.actions[index(2, c)] = make(mix_row[c]);
    }

    // Row 3: Percussion
    for (int c = 0; c < ID75_COLS; ++c)
        tl.actions[index(3, c)] = makeDrum(DRUM_NOTES[c]);

    // Row 4: System
    {
        const ActionType sys_row[15] = {
            ActionType::Panic,
            ActionType::TransposeUp, ActionType::TransposeDown,
            ActionType::OctaveUp, ActionType::OctaveDown,
            ActionType::None, ActionType::None, ActionType::None,
            ActionType::None, ActionType::None, ActionType::None,
            ActionType::None, ActionType::None, ActionType::None, ActionType::None
        };
        for (int c = 0; c < ID75_COLS; ++c)
            tl.actions[index(4, c)] = make(sys_row[c]);
    }

    // ----- Top Right (mirror of top left per sheet) -----
    tr = tl;
    tr.name = "top_right";
    tr.inverted = true;
}

// -----------------------------------------------------------------------------
// JSON I/O
// -----------------------------------------------------------------------------

bool LayoutManager::loadLayout(const std::string& filename, KeyboardLayout& layout)
{
    std::ifstream file(filename);
    if (!file.is_open())
    {
        Logger::error("Could not open layout file: " + filename);
        return false;
    }

    json j;
    try
    {
        file >> j;
    }
    catch (const json::exception& e)
    {
        Logger::error(std::string("JSON parse error in ") + filename + ": " + e.what());
        return false;
    }

    layout.name = j.value("name", std::string("unnamed"));
    layout.inverted = j.value("inverted", false);

    for (auto& a : layout.actions)
        a = Action{};

    if (!j.contains("keys") || !j["keys"].is_array())
    {
        Logger::error("Layout file missing 'keys' array: " + filename);
        return false;
    }

    for (const auto& k : j["keys"])
    {
        int row = k.value("row", -1);
        int col = k.value("col", k.value("column", -1));
        if (row < 0 || row >= ID75_ROWS || col < 0 || col >= ID75_COLS)
            continue;

        Action a;
        a.type = stringToActionType(k.value("action", std::string("none")));
        a.cents = k.value("cents", 0);
        a.semitone = k.value("semitone", 0);
        a.drum_note = k.value("drum_note", 0);
        layout.actions[index(row, col)] = a;
    }

    Logger::info("Loaded layout '" + layout.name + "' from " + filename);
    return true;
}

bool LayoutManager::saveLayout(const std::string& filename, const KeyboardLayout& layout) const
{
    json j;
    j["name"] = layout.name;
    j["inverted"] = layout.inverted;
    j["keys"] = json::array();

    for (int r = 0; r < ID75_ROWS; ++r)
    {
        for (int c = 0; c < ID75_COLS; ++c)
        {
            const Action& a = layout.actions[index(r, c)];
            if (a.type == ActionType::None)
                continue;

            json k;
            k["row"] = r;
            k["col"] = c;
            k["action"] = actionTypeToString(a.type);
            if (a.type == ActionType::Note)
                k["cents"] = a.cents;
            if (a.type == ActionType::Drum)
                k["drum_note"] = a.drum_note;
            if (a.semitone != 0)
                k["semitone"] = a.semitone;
            j["keys"].push_back(k);
        }
    }

    std::ofstream file(filename);
    if (!file.is_open())
        return false;

    file << j.dump(2) << "\n";
    return true;
}

bool LayoutManager::loadAll(const std::string& layouts_dir)
{
    KeyboardLayout built[4];
    buildPhysicalLayoutV1(built[2], built[0], built[3], built[1]);
    // indices: 0=BR, 1=TR, 2=BL, 3=TL

    bool any_from_disk = false;

    for (int i = 0; i < 4; ++i)
    {
        BoardPosition pos = static_cast<BoardPosition>(i);
        std::string path = layouts_dir + "/" + positionToFileStem(pos) + ".json";

        KeyboardLayout loaded;
        if (loadLayout(path, loaded))
        {
            layouts_[i] = loaded;
            any_from_disk = true;
        }
        else
        {
            Logger::warning("Using built-in Physical Layout V1 for " +
                            std::string(positionToFileStem(pos)));
            layouts_[i] = built[i];
        }
    }

    loaded_ = true;
    Logger::info(any_from_disk
        ? "Layouts loaded (disk + fallbacks)."
        : "All layouts from built-in Physical Layout V1.");
    return true;
}

void LayoutManager::buildDefaults()
{
    KeyboardLayout bl, br, tl, tr;
    buildPhysicalLayoutV1(bl, br, tl, tr);
    layouts_[static_cast<int>(BoardPosition::BottomLeft)]  = bl;
    layouts_[static_cast<int>(BoardPosition::BottomRight)] = br;
    layouts_[static_cast<int>(BoardPosition::TopLeft)]     = tl;
    layouts_[static_cast<int>(BoardPosition::TopRight)]    = tr;
}

