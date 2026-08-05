#include "engine.h"
#include "actions.h"
#include "layout_manager.h"
#include "config_manager.h"

#include "logger.h"
#include "instrument_state.h"
#include "keyboard_manager.h"
#include "breath_controller.h"
#include "midi_engine.h"
#include "vial_controller.h"
#include "startup_manager.h"
#include "keyboard_layout.h"   // ID75_ROWS

#include <chrono>
#include <cmath>
#include <thread>
#include <iostream>

#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

namespace
{
    // TODO: point this at the real path to your Zyn instrument bank on
    // the Pi (this was /home/mjr/Instrument_2/mkb.xmz in Instrument_2).
    constexpr const char* ZYN_INSTRUMENT_FILE = "config/mkb.xmz";

    // Ported directly from Instrument_2's compute_octave_from_nod().
    int computeOctaveFromNod(int nod_value)
    {
        int octave = (nod_value * 8) / 128;

        if (octave < 0)
            octave = 0;

        if (octave > 7)
            octave = 7;

        return octave;
    }

    Engine* g_engine_instance = nullptr;

    void handleTerminationSignal(int)
    {
        if (g_engine_instance)
        {
            g_engine_instance->requestStop();
        }
    }
}


Engine::Engine()
:
running_(false)
{
    g_engine_instance = this;
}

Engine::~Engine()
{
    shutdown();

    if (g_engine_instance == this)
    {
        g_engine_instance = nullptr;
    }
}

void Engine::requestStop()
{
    running_ = false;
}

bool Engine::initialize()
{
    std::cout << "=====================================\n";
    std::cout << " Microtonal Instrument Engine\n";
    std::cout << "=====================================\n";

    Logger::info("Starting engine initialization");

    signal(SIGINT, handleTerminationSignal);
    signal(SIGTERM, handleTerminationSignal);

    state_ = std::make_unique<InstrumentState>();

    midi_ = std::make_unique<MidiEngine>();
    if (!midi_->initialize())
    {
        Logger::error("MIDI initialization failed");
        return false;
    }

    keyboard_ = std::make_unique<KeyboardManager>();
    if (!keyboard_->initialize())
    {
        Logger::error("Keyboard manager initialization failed");
        return false;
    }

    vial_ = std::make_unique<VialController>();
    if (!vial_->initialize())
    {
        // Non-fatal: lets us run (with no RGB feedback) on a bench
        // setup without the Vial-capable HID interface enumerable yet.
        // Every StartupManager RGB call becomes a harmless no-op when
        // vial_->deviceCount() is 0.
        Logger::warning("VialController initialization failed. Continuing without RGB feedback.");
    }

    // Constructed but NOT initialized here -- StartupManager owns
    // calling breath_->initialize() as part of its non-fatal
    // verifyBreathController() stage, so it can drive the magenta
    // "missing" indication around that check.
    breath_ = std::make_unique<BreathController>();

    StartupManager startup(*keyboard_, *vial_, *breath_);
    if (!startup.run())
    {
        Logger::error("Startup sequence failed.");
        return false;
    }


    // ----- Phase 3: config + layouts -----
    config_ = std::make_unique<ConfigManager>();
    config_->load("config/instrument.json");

    {
        const auto& cfg = config_->configuration();
        mix_step_ = cfg.controls.mix_step;
        transpose_step_ = cfg.controls.transpose_step;
        drum_channel_ = cfg.audio.drum_channel;
        state_->setTranspose(cfg.audio.default_transpose);
        state_->setMasterVolume(cfg.audio.default_volume);
    }

    layouts_ = std::make_unique<LayoutManager>();
    layouts_->loadAll(config_->configuration().layouts_dir);
    // ----- end Phase 3 init -----

    if (!launchZyn())
    {
        // On the Pi (appliance mode) a missing Zyn is fatal.
        // On Ubuntu desktop development (INSTRUMENT_DESKTOP=1) we continue
        // so the rest of the engine can be exercised without audio software.
        if (std::getenv("INSTRUMENT_DESKTOP") != nullptr)
        {
            Logger::warning("ZynAddSubFX not launched (desktop mode). Continuing without audio.");
        }
        else
        {
            Logger::error("Failed to launch ZynAddSubFX");
            return false;
        }
    }

    midi_->setVolume(127);

    running_ = true;

    Logger::info("Engine initialization complete");

    return true;
}

bool Engine::launchZyn()
{
    Logger::info("Launching ZynAddSubFX...");

    zyn_pid_ = fork();

    if (zyn_pid_ == 0)
    {
        execlp(
            "pw-jack", "pw-jack", "zynaddsubfx",
            "-l", ZYN_INSTRUMENT_FILE,
            "-I", "jack", "-O", "jack", "-a",
            (char*)nullptr
        );

        perror("Failed to launch zynaddsubfx");
        _exit(1);
    }

    if (zyn_pid_ < 0)
    {
        Logger::error("fork() failed while launching ZynAddSubFX.");
        return false;
    }

    Logger::info("Waiting for ZynAddSubFX to initialize (5 seconds)...");
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // NOTE: verify the exact port name PipeWire assigns to our ALSA seq
    // client before relying on this auto-patch. On the Pi, run:
    //   pw-link -o | grep Instrument_3
    // and adjust the string below if it doesn't match.
    // pw-link exit status is deliberately ignored: these are best-effort
    // auto-patch attempts, not required for the process to run (you can
    // always patch manually with pw-link/qpwgraph if one of these
    // doesn't match your PipeWire graph).
    (void)system(
        "pw-link zynaddsubfx:out_1 "
        "$(pw-link -i | grep 'playback_FL' | grep -v 'hdmi' | head -n 1) "
        "2>/dev/null"
    );

    (void)system(
        "pw-link zynaddsubfx:out_2 "
        "$(pw-link -i | grep 'playback_FR' | grep -v 'hdmi' | head -n 1) "
        "2>/dev/null"
    );

    (void)system(
        "pw-link 'Instrument_3:MIDI Output' zynaddsubfx:midi_input 2>/dev/null"
    );

    return true;
}

void Engine::run()
{
    Logger::info("Entering performance loop.");

    std::vector<KeyEvent> events;
    int last_sent_volume = -1;

    while (running_)
    {
        if (breath_)
        {
            breath_->update();

            int breath_value = breath_->breathValue();
            int volume = (breath_value < 12) ? 0 : breath_value;

            if (volume != last_sent_volume)
            {
                midi_->setVolume(volume);
                last_sent_volume = volume;
            }

            state_->setOctave(computeOctaveFromNod(breath_->nodValue()));
        }

        keyboard_->pollPerformance(events);

        for (const auto& event : events)
        {
            handleKeyEvent(event);
        }
    }

    Logger::info("Performance loop exited.");
}


void Engine::shutdown()
{
    Logger::info("Shutting down engine");

    running_ = false;

    if (zyn_pid_ > 0)
    {
        kill(zyn_pid_, SIGTERM);

        int status = 0;
        waitpid(zyn_pid_, &status, 0);

        zyn_pid_ = -1;
    }

    if (breath_) breath_->shutdown();
    if (keyboard_) keyboard_->shutdown();
    if (midi_) midi_->shutdown();
    if (vial_) vial_->shutdown();

    Logger::info("Engine shutdown complete");
}

// ===== Phase 3 handlers (integrated) =====

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

// ===== Fixed: was missing after phase3 integrate (const qualifier) =====
void Engine::centsToMidi(int absolute_cents, int& midi_note, int& residual_cents) const
{
    // absolute_cents = octave*1200 + transpose*100 + layout_cents
    // Map to nearest MIDI note; residual is pitch-bend in cents.
    double midi_float = absolute_cents / 100.0;
    int note = static_cast<int>(std::lround(midi_float));
    residual_cents = absolute_cents - note * 100;

    if (note < 0)   note = 0;
    if (note > 127) note = 127;

    midi_note = note;
}
