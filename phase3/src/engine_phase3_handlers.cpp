// =============================================================================
// Phase 3 Engine fragments — merge into src/engine.cpp
//
// These are the new/replaced methods for absolute-cents notes, layout lookup,
// and upper-keyboard Action handling. See INTEGRATION.md for where they go.
// =============================================================================

#include "engine.h"
#include "instrument_state.h"
#include "keyboard_manager.h"
#include "midi_engine.h"
#include "layout_manager.h"
#include "config_manager.h"
#include "logger.h"
#include "keyboard_layout.h"

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// centsToMidi
// ---------------------------------------------------------------------------
// absolute_cents is layout cents + octave*1200 + transpose*100.
// We pick the nearest MIDI note and return the residual for pitch bend.
// ---------------------------------------------------------------------------
void Engine::centsToMidi(int absolute_cents, int& midi_note, int& residual_cents) const
{
    // 0 cents = MIDI note 0 (C-1) for pure math; we offset so that
    // layout 0 + octave 5*1200 = C5 region depending on transpose.
    // Practical approach: treat (absolute_cents / 100) as a floating MIDI
    // note number, round to nearest int, residual is the bend.
    double midi_float = absolute_cents / 100.0;
    int note = static_cast<int>(std::lround(midi_float));
    residual_cents = absolute_cents - note * 100;

    if (note < 0)   note = 0;
    if (note > 127) note = 127;

    midi_note = note;
}

void Engine::allNotesOff()
{
    for (const auto& kv : active_keys_)
    {
        midi_->sendNoteOff(kv.second.channel, kv.second.note);
    }
    active_keys_.clear();
    note_ref_counts_.clear();

    // Also send CC 123 (All Notes Off) on every channel as a safety net.
    for (int ch = 0; ch < 16; ++ch)
        midi_->sendControlChange(ch, 123, 0);
}

// ---------------------------------------------------------------------------
// handleKeyEvent — Phase 3: look up Action from LayoutManager
// ---------------------------------------------------------------------------
void Engine::handleKeyEvent(const KeyEvent& event)
{
    const auto& keymap = keyboard_->keymap();
    auto it = keymap.find(event.keycode);
    if (it == keymap.end())
        return;

    const Key& key = it->second;

    const auto& boards = keyboard_->keyboards();
    if (event.keyboard_index < 0 ||
        event.keyboard_index >= static_cast<int>(boards.size()))
        return;

    const Keyboard& kb = boards[event.keyboard_index];
    if (!kb.assigned || kb.logical_index < 0)
        return;

    if (key.row < 0 || key.row >= ID75_ROWS)
        return;

    auto pos = LayoutManager::indexToPosition(kb.logical_index);

    // Column as reported by keymap; LayoutManager::actionFor applies
    // the board's inverted flag itself.
    Action action = layouts_->actionFor(pos, key.row, key.column);
    action.pressed = event.pressed;

    handleAction(action, event.keyboard_index, event.keycode, event.pressed);
}

// ---------------------------------------------------------------------------
// handleAction
// ---------------------------------------------------------------------------
void Engine::handleAction(const Action& action,
                          int keyboard_index,
                          int keycode,
                          bool pressed)
{
    // Continuous / toggle controls act on press only.
    const bool is_note_like =
        action.type == ActionType::Note || action.type == ActionType::Drum;

    if (!is_note_like && !pressed)
        return;

    switch (action.type)
    {
        case ActionType::None:
            return;

        // -------------------- Notes (absolute cents) --------------------
        case ActionType::Note:
        {
            // Total cents from C-1: octave register + transpose + layout pitch.
            int absolute =
                state_->octave() * 1200 +
                state_->transpose() * 100 +
                action.cents;

            int midi_note = 0;
            int residual = 0;
            centsToMidi(absolute, midi_note, residual);

            // Channel: keep Phase-1 grouping (right boards 0-4, left 5-9)
            // via channel_offset set at assignment time.
            const auto& boards = keyboard_->keyboards();
            const Keyboard& kb = boards[keyboard_index];
            // Use row from the physical key so each row can still sit on
            // its own MIDI channel (pitch bend is per-channel).
            // Find row by reverse-looking the keycode — simpler: pass row
            // through Action in a later refinement. For now use channel_offset
            // alone for polyphony across boards; residual bend is global per ch.
            int channel = kb.channel_offset; // 0 or 5; refine with row if needed

            uint32_t key_id =
                (static_cast<uint32_t>(keyboard_index) << 16) |
                static_cast<uint32_t>(keycode);

            uint32_t note_id =
                (static_cast<uint32_t>(channel) << 8) |
                static_cast<uint32_t>(midi_note);

            if (pressed)
            {
                if (active_keys_.count(key_id))
                    return; // autorepeat

                midi_->sendNoteOn(channel, midi_note, 100, residual);
                active_keys_[key_id] = {midi_note, channel};
                note_ref_counts_[note_id]++;
            }
            else
            {
                auto held = active_keys_.find(key_id);
                if (held == active_keys_.end())
                    return;

                uint32_t held_note_id =
                    (static_cast<uint32_t>(held->second.channel) << 8) |
                    static_cast<uint32_t>(held->second.note);

                if (--note_ref_counts_[held_note_id] <= 0)
                {
                    midi_->sendNoteOff(held->second.channel, held->second.note);
                    note_ref_counts_.erase(held_note_id);
                }
                active_keys_.erase(held);
            }
            return;
        }

        // -------------------- Drums --------------------
        case ActionType::Drum:
        {
            uint32_t key_id =
                (static_cast<uint32_t>(keyboard_index) << 16) |
                static_cast<uint32_t>(keycode);

            int channel = drum_channel_;
            int note = action.drum_note;

            if (pressed)
            {
                if (active_keys_.count(key_id))
                    return;
                midi_->sendNoteOn(channel, note, 100, 0);
                active_keys_[key_id] = {note, channel};
            }
            else
            {
                auto held = active_keys_.find(key_id);
                if (held == active_keys_.end())
                    return;
                midi_->sendNoteOff(held->second.channel, held->second.note);
                active_keys_.erase(held);
            }
            return;
        }

        // -------------------- Transpose / Octave --------------------
        case ActionType::TransposeUp:
            state_->adjustTranspose(transpose_step_);
            Logger::info("Transpose → " + std::to_string(state_->transpose()));
            return;

        case ActionType::TransposeDown:
            state_->adjustTranspose(-transpose_step_);
            Logger::info("Transpose → " + std::to_string(state_->transpose()));
            return;

        case ActionType::OctaveUp:
            state_->adjustOctave(1);
            Logger::info("Octave → " + std::to_string(state_->octave()));
            return;

        case ActionType::OctaveDown:
            state_->adjustOctave(-1);
            Logger::info("Octave → " + std::to_string(state_->octave()));
            return;

        // -------------------- Master volume --------------------
        case ActionType::VolumeUp:
        case ActionType::MasterVolUp:
            state_->adjustMasterVolume(mix_step_);
            midi_->setVolume(state_->masterVolume());
            Logger::info("Master volume → " + std::to_string(state_->masterVolume()));
            return;

        case ActionType::VolumeDown:
        case ActionType::MasterVolDown:
            state_->adjustMasterVolume(-mix_step_);
            midi_->setVolume(state_->masterVolume());
            Logger::info("Master volume → " + std::to_string(state_->masterVolume()));
            return;

        // -------------------- Mixer (state only until Phase 4 audio) --------------------
        case ActionType::DryMixUp:
            state_->adjustDryMix(mix_step_);
            Logger::info("Dry mix → " + std::to_string(state_->dryMix()));
            return;
        case ActionType::DryMixDown:
            state_->adjustDryMix(-mix_step_);
            Logger::info("Dry mix → " + std::to_string(state_->dryMix()));
            return;
        case ActionType::VocoderMixUp:
            state_->adjustVocoderMix(mix_step_);
            Logger::info("Vocoder mix → " + std::to_string(state_->vocoderMix()));
            return;
        case ActionType::VocoderMixDown:
            state_->adjustVocoderMix(-mix_step_);
            Logger::info("Vocoder mix → " + std::to_string(state_->vocoderMix()));
            return;
        case ActionType::DrumMixUp:
            state_->adjustDrumMix(mix_step_);
            Logger::info("Drum mix → " + std::to_string(state_->drumMix()));
            return;
        case ActionType::DrumMixDown:
            state_->adjustDrumMix(-mix_step_);
            Logger::info("Drum mix → " + std::to_string(state_->drumMix()));
            return;

        // -------------------- Modes --------------------
        case ActionType::ModeInstrument:
            state_->setMode(PerformanceMode::SynthOnly);
            Logger::info("Mode → SynthOnly (Mode 1)");
            return;
        case ActionType::ModeVocoder:
            state_->setMode(PerformanceMode::VocoderOnly);
            Logger::info("Mode → VocoderOnly (Mode 2)");
            return;
        case ActionType::ModeVocoderDry:
            state_->setMode(PerformanceMode::SynthAndVocoder);
            Logger::info("Mode → SynthAndVocoder (Mode 3)");
            return;

        // -------------------- Panic --------------------
        case ActionType::Panic:
            Logger::warning("PANIC — all notes off");
            allNotesOff();
            state_->requestPanic();
            return;

        // -------------------- Looper (stub until Phase 4) --------------------
        case ActionType::LoopRecord:
        case ActionType::LoopPlay:
        case ActionType::LoopStop:
        case ActionType::LoopOverdub:
        case ActionType::LoopUndo:
        case ActionType::LoopClear:
            Logger::info(std::string("Looper command: ") + actionTypeToString(action.type) +
                         " (Phase 4 will send OSC/MIDI)");
            return;

        // -------------------- Effects (stub) --------------------
        case ActionType::ReverbToggle:
        case ActionType::DelayToggle:
        case ActionType::ChorusToggle:
            Logger::info(std::string("Effect toggle: ") + actionTypeToString(action.type));
            return;

        case ActionType::User1:
        case ActionType::User2:
        case ActionType::User3:
        case ActionType::User4:
            Logger::info(std::string("User action: ") + actionTypeToString(action.type));
            return;
    }
}
