#include "engine.h"
#include "actions.h"
#include "audio_graph_manager.h"
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
    std::cout << "  Microtonal Instrument Engine\n";
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
        // Non-fatal: bench setups without Vial HID still run (no RGB).
        Logger::warning("VialController initialization failed. Continuing without RGB feedback.");
    }

    // Constructed but NOT initialized here — StartupManager owns
    // breath_->initialize() inside verifyBreathController().
    breath_ = std::make_unique<BreathController>();

    StartupManager startup(*keyboard_, *vial_, *breath_);
    if (!startup.run())
    {
        Logger::error("Startup sequence failed.");
        return false;
    }

    // ----- Phase 3: config + layouts -----
    config_ = std::make_unique<ConfigManager>();
    if (!config_->load("config/instrument.json"))
    {
        Logger::warning("Config load failed; using built-in defaults.");
    }
    {
        const auto& cfg = config_->configuration();
        mix_step_ = cfg.controls.mix_step;
        transpose_step_ = cfg.controls.transpose_step;
        drum_channel_ = cfg.audio.drum_channel;
        state_->setTranspose(cfg.audio.default_transpose);
        state_->setMasterVolume(cfg.audio.default_volume);
    }

    layouts_ = std::make_unique<LayoutManager>();
    {
        const auto& cfg = config_->configuration();
        const std::string dir =
            cfg.layouts_dir.empty() ? "config/layouts" : cfg.layouts_dir;
        if (!layouts_->loadAll(dir))
        {
            Logger::info("Layout JSON missing or incomplete — building Physical Layout V1.");
            layouts_->buildDefaults();
        }
        else
        {
            Logger::info("Layouts loaded from JSON.");
        }
    }

    // ----- Phase 4: audio graph owns Zyn / SL / vocoder (single process owner) -----
    audio_ = std::make_unique<AudioGraphManager>();
    {
        AudioGraphManager::Config acfg;
        if (config_)
        {
            const auto& cfg = config_->configuration();
            if (!cfg.audio.zyn_instrument.empty())
                acfg.zyn_instrument = cfg.audio.zyn_instrument;
        }
        acfg.mic_name_hint = "MVX2U";
        audio_->setConfig(acfg);
    }

    if (!audio_->startProcesses())
    {
        // On the Pi a missing Zyn is fatal. On Ubuntu desktop development
        // (INSTRUMENT_DESKTOP=1) continue so the rest of the engine can be
        // exercised without audio software.
        if (std::getenv("INSTRUMENT_DESKTOP") != nullptr)
        {
            Logger::warning("AudioGraphManager failed to start Zyn (desktop mode). "
                            "Continuing without audio.");
        }
        else
        {
            Logger::error("Failed to launch ZynAddSubFX via AudioGraphManager");
            return false;
        }
    }
    else
    {
        audio_->setMode(PerformanceMode::SynthOnly);
        Logger::info(std::string("Audio graph: ") + audio_->statusSummary());
    }

    {
        auto mic = audio_->queryMic();
        if (mic.present)
        {
            Logger::info("Shure MVX2U mic detected"
                         + (mic.alsa_hint.empty() ? "" : (" ALSA " + mic.alsa_hint))
                         + " ports=" + std::to_string(mic.capture_ports.size()));
            for (const auto& p : mic.capture_ports)
                Logger::info("  capture: " + p);
        }
        else
        {
            Logger::info("Shure MVX2U mic not detected (OK for Mode 1)");
        }
    }

    midi_->setVolume(state_ ? state_->masterVolume() : 127);
    running_ = true;
    Logger::info("Engine initialization complete");
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
    if (audio_) audio_->stopProcesses();
    Logger::info("Shutting down engine");

    running_ = false;


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
    {
        return;
    }

    const Keyboard& kb = boards[event.keyboard_index];

    // Map physical keyboard index -> BoardPosition via assignment order
    // (0=BR, 1=TR, 2=BL, 3=TL) which matches LayoutManager::indexToPosition.
    LayoutManager::BoardPosition pos =
        LayoutManager::indexToPosition(kb.logical_index >= 0 ? kb.logical_index
                                                              : event.keyboard_index);

    Action action = layouts_->actionFor(pos, key.row, key.column);
    if (action.type == ActionType::None)
        return;

    handleAction(action, event.keyboard_index, event.keycode, event.pressed);
}

void Engine::centsToMidi(int absolute_cents, int& midi_note, int& residual_cents) const
{
    double midi_float = absolute_cents / 100.0;
    int note = static_cast<int>(std::lround(midi_float));
    residual_cents = absolute_cents - note * 100;

    if (note < 0)   note = 0;
    if (note > 127) note = 127;

    midi_note = note;
}

void Engine::handleAction(const Action& action,
                          int keyboard_index,
                          int keycode,
                          bool pressed)
{
    const bool is_note_like =
        action.type == ActionType::Note || action.type == ActionType::Drum;

    if (!is_note_like && !pressed)
        return;

    switch (action.type)
    {
        case ActionType::None:
            return;

        case ActionType::Note:
        {
            int absolute =
                state_->octave() * 1200 +
                state_->transpose() * 100 +
                action.cents;

            int midi_note = 0;
            int residual = 0;
            centsToMidi(absolute, midi_note, residual);

            const auto& boards = keyboard_->keyboards();
            const Keyboard& kb = boards[keyboard_index];
            int channel = kb.channel_offset;

            uint32_t key_id =
                (static_cast<uint32_t>(keyboard_index) << 16) |
                static_cast<uint32_t>(keycode);

            uint32_t note_id =
                (static_cast<uint32_t>(channel) << 8) |
                static_cast<uint32_t>(midi_note);

            if (pressed)
            {
                if (active_keys_.count(key_id))
                    return;

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

        case ActionType::Drum:
        {
            int channel = drum_channel_;
            int note = action.drum_note;

            uint32_t key_id =
                (static_cast<uint32_t>(keyboard_index) << 16) |
                static_cast<uint32_t>(keycode);

            uint32_t note_id =
                (static_cast<uint32_t>(channel) << 8) |
                static_cast<uint32_t>(note);

            if (pressed)
            {
                if (active_keys_.count(key_id))
                    return;
                midi_->sendNoteOn(channel, note, 100, 0);
                active_keys_[key_id] = {note, channel};
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

        case ActionType::TransposeUp:
            state_->adjustTranspose(transpose_step_);
            Logger::info("Transpose " + std::to_string(state_->transpose()));
            return;
        case ActionType::TransposeDown:
            state_->adjustTranspose(-transpose_step_);
            Logger::info("Transpose " + std::to_string(state_->transpose()));
            return;

        case ActionType::OctaveUp:
            state_->adjustOctave(1);
            Logger::info("Octave " + std::to_string(state_->octave()));
            return;
        case ActionType::OctaveDown:
            state_->adjustOctave(-1);
            Logger::info("Octave " + std::to_string(state_->octave()));
            return;

        case ActionType::VolumeUp:
            state_->adjustMasterVolume(mix_step_);
            midi_->setVolume(state_->masterVolume());
            Logger::info("Master volume " + std::to_string(state_->masterVolume()));
            return;
        case ActionType::VolumeDown:
            state_->adjustMasterVolume(-mix_step_);
            midi_->setVolume(state_->masterVolume());
            Logger::info("Master volume " + std::to_string(state_->masterVolume()));
            return;

        case ActionType::MasterVolUp:
            state_->adjustMasterVolume(mix_step_);
            midi_->setVolume(state_->masterVolume());
            Logger::info("Master volume " + std::to_string(state_->masterVolume()));
            return;
        case ActionType::MasterVolDown:
            state_->adjustMasterVolume(-mix_step_);
            midi_->setVolume(state_->masterVolume());
            Logger::info("Master volume " + std::to_string(state_->masterVolume()));
            return;

        case ActionType::DryMixUp:
            state_->adjustDryMix(mix_step_);
            Logger::info("Dry mix " + std::to_string(state_->dryMix()));
            if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
            return;
        case ActionType::DryMixDown:
            state_->adjustDryMix(-mix_step_);
            Logger::info("Dry mix " + std::to_string(state_->dryMix()));
            if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
            return;
        case ActionType::VocoderMixUp:
            state_->adjustVocoderMix(mix_step_);
            Logger::info("Vocoder mix " + std::to_string(state_->vocoderMix()));
            if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
            return;
        case ActionType::VocoderMixDown:
            state_->adjustVocoderMix(-mix_step_);
            Logger::info("Vocoder mix " + std::to_string(state_->vocoderMix()));
            if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
            return;
        case ActionType::DrumMixUp:
            state_->adjustDrumMix(mix_step_);
            Logger::info("Drum mix " + std::to_string(state_->drumMix()));
            if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
            return;
        case ActionType::DrumMixDown:
            state_->adjustDrumMix(-mix_step_);
            Logger::info("Drum mix " + std::to_string(state_->drumMix()));
            if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
            return;

        case ActionType::ModeInstrument:
            state_->setMode(PerformanceMode::SynthOnly);
            if (audio_) audio_->setMode(PerformanceMode::SynthOnly);
            Logger::info("Mode SynthOnly (Mode 1)");
            return;
        case ActionType::ModeVocoder:
            state_->setMode(PerformanceMode::VocoderOnly);
        if (audio_) audio_->setMode(PerformanceMode::VocoderOnly);
            Logger::info("Mode VocoderOnly (Mode 2)");
            return;
        case ActionType::ModeVocoderDry:
            state_->setMode(PerformanceMode::SynthAndVocoder);
        if (audio_) audio_->setMode(PerformanceMode::SynthAndVocoder);
            Logger::info("Mode SynthAndVocoder (Mode 3)");
            return;

        case ActionType::Panic:
            Logger::warning("PANIC — all notes off");
            allNotesOff();
            state_->requestPanic();
            return;

        case ActionType::LoopRecord:
        case ActionType::LoopPlay:
        case ActionType::LoopStop:
        case ActionType::LoopOverdub:
        case ActionType::LoopUndo:
        case ActionType::LoopClear:
            switch (action.type)
            {
                case ActionType::LoopRecord:  if (audio_) audio_->looperRecord();  break;
                case ActionType::LoopPlay:    if (audio_) audio_->looperPlay();    break;
                case ActionType::LoopStop:    if (audio_) audio_->looperStop();    break;
                case ActionType::LoopOverdub: if (audio_) audio_->looperOverdub(); break;
                case ActionType::LoopUndo:    if (audio_) audio_->looperUndo();    break;
                case ActionType::LoopClear:   if (audio_) audio_->looperClear();   break;
                default: break;
            }
            return;

        case ActionType::ReverbToggle:
        case ActionType::DelayToggle:
        case ActionType::ChorusToggle:
            Logger::info(std::string("Effect toggle: ") +
                         actionTypeToString(action.type));
            return;

        case ActionType::User1:
        case ActionType::User2:
        case ActionType::User3:
        case ActionType::User4:
            Logger::info(std::string("User action: ") +
                         actionTypeToString(action.type));
            return;

        default:
            return;
    }
}
