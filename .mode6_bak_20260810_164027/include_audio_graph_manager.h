#pragma once

#include "instrument_state.h"  // PerformanceMode

#include <atomic>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

//
// Owns external audio processes and the PipeWire graph.
//
// Mode 1 (SynthOnly):       Zyn → system playback
// Mode 2 (VocoderOnly):     Zyn (carrier) + mic (modulator) → Calf Vocoder → playback
// Mode 3 (SynthAndVocoder): Mode 2 + dry mic path with independent level
// Mode 4 (BreathOctave): Mode 1 routing; breath drives octave only
// Mode 5 (BreathOctaveMic): Mode 4 + dry mic → SL + headphones
//
// Looper (SooperLooper) sits on a side chain and is controlled via OSC;
// it is not driven by raw keyboard events.
//
// Design constraints:
//   - All process launch / kill / pw-link lives here (not in Engine).
//   - Ubuntu and Raspberry Pi both use PipeWire; prefer pw-link / pw-cli.
//   - Missing tools (no sooperlooper, no calf, no mic) must never crash
//     the instrument — log + degrade gracefully.
//
class AudioGraphManager
{
public:
    struct Config
    {
        std::string zyn_instrument = "config/mkb.xmz";
        std::string zyn_drums_instrument = "config/drums.xmz";
        std::string zyn_drums_client_name = "zyn-drums";
        std::string zyn_client_name = "zyn-melody";
        std::string engine_midi_name = "Midi-Bridge:Instrument_3MIDI Output (capture)";

        // ALSA / PipeWire name fragments used to find the Shure MVX2U.
        std::string mic_name_hint = "MVX2U";

        // Optional absolute paths; empty → search PATH.
        std::string zyn_binary;
        std::string sooperlooper_binary;
        std::string jalv_binary;          // for Calf LV2 via jalv.gtk / jalv

        // Calf Vocoder LV2 URI (common default; override if your install differs).
        std::string calf_vocoder_uri =
            "http://calf.sourceforge.net/plugins/Vocoder";

        // SooperLooper OSC port (default SL port).
        int sooperlooper_osc_port = 9951;
    };

    AudioGraphManager();
    ~AudioGraphManager();

    AudioGraphManager(const AudioGraphManager&) = delete;
    AudioGraphManager& operator=(const AudioGraphManager&) = delete;

    void setConfig(const Config& cfg);
    const Config& config() const { return cfg_; }

    // Launch Zyn (and later SL / vocoder hosts). Safe to call more than once;
    // subsequent calls are no-ops if the process is still alive.
    bool startProcesses();

    // Tear everything down: unlink, kill children, wait.
    void stopProcesses();

    // Build / rebuild the PipeWire graph for the requested mode.
    // Returns false only on hard failure (e.g. Zyn not running for Mode 1).
    bool setMode(PerformanceMode mode);
 // Every ~2s from Engine: refresh entity ports, relaunch dead
 // processes, rebuild current-mode wiring. Safe to call often.
 void ensureHealthyGraph();


    PerformanceMode mode() const { return mode_; }

    // Apply mixer levels (0–127) to whatever gain nodes we currently own.
    // Phase 4.1: master → Zyn-facing CC path is left to Engine/MidiEngine;
    //            dry/vocoder levels are logged until real nodes exist.
    void applyMixerLevels(int master, int dry, int vocoder, int drums,
 int synth = 100, int mic = 100);

    // True if an input port matching mic_name_hint is visible to PipeWire/ALSA.
    bool micPresent() const;

    // Human-readable status for logs / diagnostics.
    std::string statusSummary() const;

    bool zynRunning() const;
    bool drumsZynRunning() const;
    bool sooperlooperRunning() const;
    bool vocoderRunning() const;

    // --- Looper OSC commands (Phase 4.3; stubs log until SL is up) ---
    void looperRecord();
 void looperOverdub();
 void looperPlay();
 void looperStop();
 void looperUndo();
 void looperClear();
 // --- Per-loop control (4 independent loops, track index 0..3) ---
 void looperRecordTrack(int track);
 void looperMuteTrack(int track);
 void looperClearTrack(int track);
 // Mutes all loops only -- never the live Zyn output.
 void looperMuteAll();
    struct MicInfo
    {
        bool present = false;
        std::string alsa_card_name;     // e.g. "MVX2U"
        std::string alsa_hint;          // e.g. "hw:3,0" if known
        std::vector<std::string> capture_ports;   // PipeWire input ports
        std::vector<std::string> monitor_ports;   // optional monitor outs
    };

    // Snapshot of what we currently believe about the Shure interface.
    MicInfo queryMic() const;
    // If ALSA sees the mic but PipeWire has no capture ports,
    // force a capture-capable card profile (wpctl/pactl). Throttled.
    // Returns true if capture ports are visible after the attempt.
    bool activateMicCapturePorts();

    // Ports whose node/port name contains any of the needles.
    std::vector<std::string> findPortsMatching(
        const std::vector<std::string>& needles,
        bool inputs) const;
    // Link SooperLooper into the current graph (dry/wet parallel path).
    // Safe no-op if SL is not running or ports are missing.
    void connectLooperGraph();
    bool resolveLooperPorts(std::string& in_l, std::string& in_r,
                            std::string& out_l, std::string& out_r);
    void linkStereoToLooper(const std::string& src_l, const std::string& src_r,
                            const std::string& sl_in_l, const std::string& sl_in_r);
    std::string findEngineMidiPort() const;
    std::string findEngineDrumsMidiPort() const;

    // Send one OSC message to SooperLooper (uses oscsend from liblo-tools).
    bool sendLooperOsc(const std::string& path,
                       const std::string& type_tags,
                       const std::string& arg) const;
    // SooperLooper property set: /sl/N/set  ssf  <name> <float>
    bool sendLooperOscSet(int track, const std::string& prop, float value) const;
    // Master = first recorded track until all tracks cleared.
    void applyLooperSyncTopology();
    void realignActiveLoops();
    void noteLoopRecordArmed(int track);
    void noteLoopCleared(int track);
    void noteAllLoopsCleared();











private:

    // 0.0-1.0 linear gain on a PipeWire node via wpctl or pw-cli (best-effort).
    bool setNodeVolume(const std::string& node_name_substring, float linear);
    // Same idea, but for a capture SOURCE (mic/input), not a playback sink --
    // setNodeVolume() alone can never find a USB mic, since it only queries sinks.
    bool setSourceVolume(const std::string& node_name_substring, float linear);

    // Cache last mixer values so mode rebuilds can re-apply.
    int last_master_  = 127;
    int last_dry_     = 100;
    int last_vocoder_ = 100;
    int last_drums_ = 100;
 int last_synth_ = 100;
 int last_mic_ = 100;
 // Shared target for Overdub/Undo; armed by Record on that loop track.
 int armed_loop_ = 0;
    // Dynamic cycle master: -1 = none (next Record becomes master).
    int cycle_master_ = -1;
    bool loop_active_[4] = {false, false, false, false};
    bool loop_recording_[4] = {false, false, false, false};
    double master_cycle_sec_ = 0.0;
    double master_epoch_sec_ = 0.0; // steady-clock secs of a known master downbeat
    double record_open_sec_[4] = {0, 0, 0, 0};


    bool launchZyn();
    bool launchZynDrums();
    bool launchSooperLooper();
    bool launchVocoder();

    void killPid(pid_t& pid, const char* label);
    bool processAlive(pid_t pid) const;

    // PipeWire helpers (pw-link / pw-cli). All failures are soft.
    bool pwLink(const std::string& src, const std::string& dst);
    bool pwUnlink(const std::string& src, const std::string& dst);
    void disconnectAllOwnedLinks();
    void forceDisconnectInstrumentGraph();

    bool buildMode1();
    bool buildMode2();
    bool buildMode3();
 bool buildMode4();
 bool buildMode5();

    // Discover the real PipeWire name of our ALSA MIDI sequencer
    // output (MidiEngine client "Instrument_3", port "MIDI Output").
    // Bridge naming varies (e.g. "Midi-Bridge:Instrument_3MIDI Output
    // (capture)" vs "Instrument_3:MIDI Output"); look it up at runtime.
    

    // Find PipeWire port names containing a substring (empty if none).
    std::vector<std::string> findPorts(const std::string& substring,
                                       bool inputs) const;

    // Real PipeWire name for a JACK client started with -N <name>.
    // e.g. -N zyn-melody → zynaddsubfx_zyn-melody:out_1 (not zyn-melody:out_1).
    std::string resolveClientPort(const std::string& client_name,
                                 const std::string& suffix,
                                 bool inputs) const;


    Config cfg_;
    PerformanceMode mode_ = PerformanceMode::SynthOnly;

    pid_t zyn_pid_ = -1;
    pid_t zyn_drums_pid_ = -1;
    pid_t sl_pid_  = -1;
    pid_t voc_pid_ = -1;

    // Links we created, so we can tear them down without touching unrelated
    // session links.
    std::vector<std::pair<std::string, std::string>> owned_links_;

    std::atomic<bool> started_{false};
};
