#include "engine.h"
#include "actions.h"
#include "audio_graph_manager.h"
#include "layout_manager.h"
#include "config_manager.h"

#include "logger.h"
#include "instrument_state.h"
#include "keyboard_manager.h"
#include "breath_controller.h"
#include "usb_topology.h"
#include "midi_engine.h"
#include "vial_controller.h"
#include "startup_manager.h"
#include "status_colors.h"
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
        breath_volume_cc_ = cfg.controls.breath_volume_cc;
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

    // Master volume is a stored ceiling only -- it is not pushed to
    // MIDI directly. Actual instrument loudness is breath pressure
    // scaled by this ceiling, each tick, in Engine::run().
    running_ = true;
    Logger::info("Engine initialization complete");
    return true;
}





void Engine::run()
{
    // Hot-plug support state
    auto next_breath_retry = std::chrono::steady_clock::now();
    auto breath_led_off_at = std::chrono::steady_clock::now();
    bool breath_led_active = false;

    Logger::info("Entering performance loop.");
    std::vector<KeyEvent> events;
    int last_sent_volume = -1;

    while (running_)
    {
        auto now = std::chrono::steady_clock::now();

        // ===== HOTPLUG STATUS (keyboards / mic / breath / graph) =====
        static auto next_kb_check = std::chrono::steady_clock::now();
        static auto next_mic_check = std::chrono::steady_clock::now();
        static auto next_graph_check = std::chrono::steady_clock::now();
        static bool kb_fault = false;
        static bool mic_fault = false;
        static bool breath_fault = false;
        static bool awaiting_kb_reassign = false;
        static bool assign_callback_armed = false;
        static int last_live_count = 4;

        auto paint_unassigned = [&](const VialController::Color& color) {
            if (!keyboard_ || !vial_) return;
            vial_->rescan();
            for (auto& kb : keyboard_->keyboards()) {
                kb.rgb_device_index = -1;
                if (kb.event_path.empty() || kb.fd < 0) continue;
                std::string kb_port = usbDevicePortAddress(kb.event_path);
                if (kb_port.empty()) continue;
                for (int vi = 0; vi < vial_->deviceCount(); ++vi) {
                    if (usbDevicePortAddress(vial_->devicePath(vi)) == kb_port) {
                        kb.rgb_device_index = vi;
                        break;
                    }
                }
            }
            for (const auto& kb : keyboard_->keyboards()) {
                if (kb.fd < 0 || kb.rgb_device_index < 0) continue;
                if (kb.assigned) continue;
                vial_->setColor(kb.rgb_device_index, color);
            }
            for (int vi = 0; vi < vial_->deviceCount(); ++vi) {
                bool claimed = false;
                for (const auto& kb : keyboard_->keyboards()) {
                    if (kb.rgb_device_index == vi) { claimed = true; break; }
                }
                if (!claimed)
                    vial_->setColor(vi, color);
            }
        };

        // Keyboards every 500ms
        if (keyboard_ && now >= next_kb_check) {
            next_kb_check = now + std::chrono::milliseconds(500);
            int live = keyboard_->checkLiveness();
            if (live < 4) {
                if (!kb_fault || live != last_live_count) {
                    Logger::warning(
                        "Keyboard hotplug: " + std::to_string(live) +
                        "/4 live - unassigned boards BLUE");
                    kb_fault = true;
                    awaiting_kb_reassign = true;
                    keyboard_->resetAssignments();
                    assign_callback_armed = false;
                    paint_unassigned(StatusColors::Blue);
                }
                if (!assign_callback_armed) {
                    assign_callback_armed = true;
                    keyboard_->setAssignmentCallback(
                        [this](int physical, KeyboardManager::KeyboardPosition) {
                            if (!keyboard_ || !vial_) return;
                            if (physical < 0 ||
                                physical >= static_cast<int>(keyboard_->keyboards().size()))
                                return;
                            auto& kb = keyboard_->keyboards()[physical];
                            if (kb.rgb_device_index < 0 && !kb.event_path.empty()) {
                                vial_->rescan();
                                std::string kb_port = usbDevicePortAddress(kb.event_path);
                                for (int vi = 0; vi < vial_->deviceCount(); ++vi) {
                                    if (usbDevicePortAddress(vial_->devicePath(vi)) == kb_port) {
                                        kb.rgb_device_index = vi;
                                        break;
                                    }
                                }
                            }
                            if (kb.rgb_device_index >= 0)
                                vial_->setColor(kb.rgb_device_index, StatusColors::Green);
                            Logger::info(
                                "Assigned board " + std::to_string(physical) +
                                " solid green (rgb " +
                                std::to_string(kb.rgb_device_index) + ")");
                        });
                }
            } else if (awaiting_kb_reassign) {
                if (kb_fault || last_live_count < 4) {
                    Logger::info(
                        "All 4 keyboards detected - assignment: Bottom Right, "
                        "Top Right, Bottom Left, Top Left");
                    if (kb_fault) {
                        keyboard_->resetAssignments();
                        assign_callback_armed = false;
                        kb_fault = false;
                    }
                    paint_unassigned(StatusColors::Blue);
                    if (!assign_callback_armed) {
                        assign_callback_armed = true;
                        keyboard_->setAssignmentCallback(
                            [this](int physical, KeyboardManager::KeyboardPosition) {
                                if (!keyboard_ || !vial_) return;
                                if (physical < 0 ||
                                    physical >= static_cast<int>(keyboard_->keyboards().size()))
                                    return;
                                auto& kb = keyboard_->keyboards()[physical];
                                if (kb.rgb_device_index < 0 && !kb.event_path.empty()) {
                                    vial_->rescan();
                                    std::string kb_port = usbDevicePortAddress(kb.event_path);
                                    for (int vi = 0; vi < vial_->deviceCount(); ++vi) {
                                        if (usbDevicePortAddress(vial_->devicePath(vi)) == kb_port) {
                                            kb.rgb_device_index = vi;
                                            break;
                                        }
                                    }
                                }
                                if (kb.rgb_device_index >= 0)
                                    vial_->setColor(kb.rgb_device_index, StatusColors::Green);
                                Logger::info(
                                    "Assigned board " + std::to_string(physical) +
                                    " solid green (rgb " +
                                    std::to_string(kb.rgb_device_index) + ")");
                            });
                    }
                }
            }
            last_live_count = live;
        }

        if (awaiting_kb_reassign && keyboard_) {
            keyboard_->poll();
            if (keyboard_->allAssigned()) {
                Logger::info("Keyboard re-assignment complete - LEDs off");
                awaiting_kb_reassign = false;
                kb_fault = false;
                assign_callback_armed = false;
                keyboard_->setAssignmentCallback(nullptr);
                if (vial_) {
                    for (int vi = 0; vi < vial_->deviceCount(); ++vi)
                        vial_->turnOff(vi);
                }
            }
        }

        // Mic every 2s
        if (now >= next_mic_check) {
            next_mic_check = now + std::chrono::seconds(2);
            bool mic_ok = audio_ && audio_->micPresent();
            if (!mic_ok) {
                if (!mic_fault) {
                    Logger::warning("Microphone disconnected - ORANGE");
                    mic_fault = true;
                    if (!awaiting_kb_reassign && vial_) {
                        for (int vi = 0; vi < vial_->deviceCount(); ++vi)
                            vial_->setColor(vi, StatusColors::Orange);
                    }
                }
            } else if (mic_fault) {
                Logger::info("Microphone redetected - full graph health rebuild");
                mic_fault = false;
                if (audio_)
                    audio_->ensureHealthyGraph();
                if (!awaiting_kb_reassign && !breath_fault && vial_) {
                    for (int vi = 0; vi < vial_->deviceCount(); ++vi)
                        vial_->turnOff(vi);
                }
            }
        }

        // Breath reconnect retry + GREEN while missing
        if (breath_ && !breath_->isConnected() && now >= next_breath_retry) {
            next_breath_retry = now + std::chrono::seconds(2);
            if (breath_->initialize()) {
                Logger::info("Breath controller connected (hot-plugged).");
                if (vial_) {
                    for (int vi = 0; vi < vial_->deviceCount(); ++vi)
                        vial_->setColor(vi, StatusColors::Green);
                }
                breath_led_off_at = now + std::chrono::milliseconds(400);
                breath_led_active = true;
                breath_fault = false;
            }
        }
        if (breath_led_active && now >= breath_led_off_at) {
            if (vial_ && !awaiting_kb_reassign && !mic_fault && !breath_fault) {
                for (int vi = 0; vi < vial_->deviceCount(); ++vi)
                    vial_->turnOff(vi);
            }
            breath_led_active = false;
        }
        if (breath_) {
            if (!breath_->isConnected()) {
                if (!breath_fault) {
                    Logger::warning("Breath disconnected - GREEN");
                    breath_fault = true;
                    if (!awaiting_kb_reassign && !mic_fault && vial_) {
                        for (int vi = 0; vi < vial_->deviceCount(); ++vi)
                            vial_->setColor(vi, StatusColors::Green);
                    }
                }
            } else if (breath_fault) {
                breath_fault = false;
                Logger::info("Breath restored");
                if (!awaiting_kb_reassign && !mic_fault && vial_) {
                    for (int vi = 0; vi < vial_->deviceCount(); ++vi)
                        vial_->turnOff(vi);
                }
            }
        }

        // Full audio graph health every 2s
        if (audio_ && now >= next_graph_check) {
            next_graph_check = now + std::chrono::seconds(2);
            audio_->ensureHealthyGraph();
        }
        // ===== END HOTPLUG =====

        // Breath values -> state / volume (Mode 1 only for CC7)
        if (breath_) {
            breath_->update();
            int breath_value = breath_->breathValue();
            int breath_pressure = (breath_value < 12) ? 0 : breath_value;
            state_->setBreath(breath_pressure);
            if (state_->mode() == PerformanceMode::SynthOnly) {
                int scaled_volume = (breath_pressure * state_->masterVolume()) / 127;
                if (scaled_volume != last_sent_volume) {
                    for (int ch = 0; ch < 16; ++ch)
                        midi_->sendControlChange(ch, breath_volume_cc_, scaled_volume);
                    last_sent_volume = scaled_volume;
                }
            }
            state_->setOctave(computeOctaveFromNod(breath_->nodValue()));
        }

        // Keys: skip performance polling while re-assigning
        if (!awaiting_kb_reassign)
            keyboard_->pollPerformance(events);
        else
            events.clear();
        for (const auto& event : events)
            handleKeyEvent(event);

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

    handleAction(action, event.keyboard_index, event.keycode, event.pressed, key.row);
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

void Engine::handleAction(const Action& action, int keyboard_index, int keycode, bool pressed, int row)
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
            int channel = kb.channel_offset + row; // restored: 1 of 10 chordal tracks (0-9)
                int bend_cents = residual; // layout cents are the sole microtonal source

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

                midi_->sendNoteOn(channel, midi_note, 100, bend_cents);
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
                // Drum velocity = 100 * (drumMix/127) * (masterVolume/127).
                // Never modulated by breath or vocoder gain.
                int velocity = (100 * state_->drumMix() * state_->masterVolume()) / (127 * 127);
                if (velocity < 1) velocity = 1;
                if (velocity > 127) velocity = 127;

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
                midi_->sendDrumNoteOn(channel, note, velocity);
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

       
 case ActionType::SynthVolUp:
 state_->adjustSynthMix(mix_step_);
 {
 int v = state_->synthMix();
 for (int ch = 0; ch < 16; ++ch)
 midi_->sendControlChange(ch, breath_volume_cc_, v);
 }
 if (audio_)
 audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(),
 state_->vocoderMix(), state_->drumMix(),
 state_->synthMix(), state_->micMix());
 Logger::info("Synth volume " + std::to_string((state_->synthMix() * 100) / 127) + "% (" + std::to_string(state_->synthMix()) + "/127)");

return;
 case ActionType::SynthVolDown:
 state_->adjustSynthMix(-mix_step_);
 {
 int v = state_->synthMix();
 for (int ch = 0; ch < 16; ++ch)
 midi_->sendControlChange(ch, breath_volume_cc_, v);
 }
 if (audio_)
 audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(),
 state_->vocoderMix(), state_->drumMix(),
 state_->synthMix(), state_->micMix());
 Logger::info("Synth volume " + std::to_string((state_->synthMix() * 100) / 127) + "% (" + std::to_string(state_->synthMix()) + "/127)");

return;
 case ActionType::MicVolUp:
 state_->adjustMicMix(mix_step_);
 if (audio_)
 audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(),
 state_->vocoderMix(), state_->drumMix(),
 state_->synthMix(), state_->micMix());
 Logger::info("Mic volume " + std::to_string((state_->micMix() * 100) / 127) + "% (" + std::to_string(state_->micMix()) + "/127)");

return;
 case ActionType::MicVolDown:
 state_->adjustMicMix(-mix_step_);
 if (audio_)
 audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(),
 state_->vocoderMix(), state_->drumMix(),
 state_->synthMix(), state_->micMix());
 Logger::info("Mic volume " + std::to_string((state_->micMix() * 100) / 127) + "% (" + std::to_string(state_->micMix()) + "/127)");

return;
 case ActionType::VolumeUp:
            state_->adjustMasterVolume(mix_step_);
            if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
            Logger::info("Master volume " + std::to_string(state_->masterVolume()));
            return;
        case ActionType::VolumeDown:
            state_->adjustMasterVolume(-mix_step_);
            if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
            Logger::info("Master volume " + std::to_string(state_->masterVolume()));
            return;

        case ActionType::MasterVolUp:
            state_->adjustMasterVolume(mix_step_);
            if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
            Logger::info("Master volume " + std::to_string(state_->masterVolume()));
            return;
        case ActionType::MasterVolDown:
            state_->adjustMasterVolume(-mix_step_);
            if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
            Logger::info("Master volume " + std::to_string(state_->masterVolume()));
            return;

        case ActionType::DryMixUp:
            state_->adjustDryMix(mix_step_);
if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
 Logger::info("Dry mix " + std::to_string((state_->dryMix() * 100) / 127) + "% (" + std::to_string(state_->dryMix()) + "/127)");

            return;
        case ActionType::DryMixDown:
            state_->adjustDryMix(-mix_step_);
if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
 Logger::info("Dry mix " + std::to_string((state_->dryMix() * 100) / 127) + "% (" + std::to_string(state_->dryMix()) + "/127)");

            return;
        case ActionType::VocoderMixUp:
            state_->adjustVocoderMix(mix_step_);
if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
 Logger::info("Vocoder mix " + std::to_string((state_->vocoderMix() * 100) / 127) + "% (" + std::to_string(state_->vocoderMix()) + "/127)");

            return;
        case ActionType::VocoderMixDown:
            state_->adjustVocoderMix(-mix_step_);
if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
 Logger::info("Vocoder mix " + std::to_string((state_->vocoderMix() * 100) / 127) + "% (" + std::to_string(state_->vocoderMix()) + "/127)");

            return;
        case ActionType::DrumMixUp:
            state_->adjustDrumMix(mix_step_);
if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
 Logger::info("Drum mix " + std::to_string((state_->drumMix() * 100) / 127) + "% (" + std::to_string(state_->drumMix()) + "/127)");

            return;
        case ActionType::DrumMixDown:
            state_->adjustDrumMix(-mix_step_);
if (audio_) audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(), state_->vocoderMix(), state_->drumMix());
 Logger::info("Drum mix " + std::to_string((state_->drumMix() * 100) / 127) + "% (" + std::to_string(state_->drumMix()) + "/127)");

            return;

        case ActionType::ModeInstrument:
            state_->setMode(PerformanceMode::SynthOnly);
            if (audio_) audio_->setMode(PerformanceMode::SynthOnly);
            Logger::info("Mode SynthOnly (Mode 1)");
            return;
        case ActionType::ModeVocoder:
            state_->setMode(PerformanceMode::VocoderOnly);
        if (audio_) audio_->setMode(PerformanceMode::VocoderOnly);
        // [vocoder-fix] Breath no longer controls chordal-Zyn volume in
        // Mode 2; seed a fixed full CC7 level so the vocoder carrier
        // is never silent because of wherever breath pressure last left it.
        for (int ch = 0; ch < 16; ++ch) {
            midi_->sendControlChange(ch, breath_volume_cc_, 127);
            }
            Logger::info("Mode VocoderOnly (Mode 2)");
            return;
        case ActionType::ModeVocoderDry:
            state_->setMode(PerformanceMode::SynthAndVocoder);
        if (audio_) audio_->setMode(PerformanceMode::SynthAndVocoder);
        // [vocoder-fix] Breath no longer controls chordal-Zyn volume in
        // Mode 3; seed a fixed full CC7 level so the vocoder carrier
        // is never silent because of wherever breath pressure last left it.
        for (int ch = 0; ch < 16; ++ch) {
            midi_->sendControlChange(ch, breath_volume_cc_, 127);
            }
            Logger::info("Mode SynthAndVocoder (Mode 3)");
            return;

       
 case ActionType::ModeBreathOctave:
 state_->setMode(PerformanceMode::BreathOctave);
 if (audio_) audio_->setMode(PerformanceMode::BreathOctave);
 {
 int v = state_->synthMix();
 for (int ch = 0; ch < 16; ++ch)
 midi_->sendControlChange(ch, breath_volume_cc_, v);
 }
 Logger::info("Mode BreathOctave (Mode 4) — breath=octave, synth vol via buttons");
 return;
 case ActionType::ModeBreathOctaveMic:
 state_->setMode(PerformanceMode::BreathOctaveMic);
 if (audio_) audio_->setMode(PerformanceMode::BreathOctaveMic);
 if (audio_)
 audio_->applyMixerLevels(state_->masterVolume(), state_->dryMix(),
 state_->vocoderMix(), state_->drumMix(),
 state_->synthMix(), state_->micMix());
 {
 int v = state_->synthMix();
 for (int ch = 0; ch < 16; ++ch)
 midi_->sendControlChange(ch, breath_volume_cc_, v);
 }
 Logger::info("Mode BreathOctaveMic (Mode 5) — Mode 4 + dry mic");
 return;
 case ActionType::ModeTalkbox:
 state_->setMode(PerformanceMode::Talkbox);
 if (audio_) audio_->setMode(PerformanceMode::Talkbox);
 for (int ch = 0; ch < 16; ++ch)
 midi_->sendControlChange(ch, breath_volume_cc_, 127);
 Logger::info("Mode Talkbox (Mode 6) — sawtooth → MVX2U, mic → SL + phones");
 return;
 case ActionType::Panic:
            Logger::warning("PANIC — all notes off");
            allNotesOff();
            state_->requestPanic();
            return;

        case ActionType::LoopRecord:
            case ActionType::LoopMute:
            case ActionType::LoopClear:
            case ActionType::LoopPlay:
            case ActionType::LoopStop:
            case ActionType::LoopOverdub:
            case ActionType::LoopUndo:
            case ActionType::LoopMuteAll:
                switch (action.type)
                {
                case ActionType::LoopRecord: if (audio_) audio_->looperRecordTrack(action.loop_track); break;
                case ActionType::LoopMute: if (audio_) audio_->looperMuteTrack(action.loop_track); break;
                case ActionType::LoopClear: if (audio_) audio_->looperClearTrack(action.loop_track); break;
                case ActionType::LoopPlay: if (audio_) audio_->looperPlay(); break;
                case ActionType::LoopStop: if (audio_) audio_->looperStop(); break;
                case ActionType::LoopOverdub: if (audio_) audio_->looperOverdub(); break;
                case ActionType::LoopUndo: if (audio_) audio_->looperUndo(); break;
                case ActionType::LoopMuteAll: if (audio_) audio_->looperMuteAll(); break;
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
