#pragma once

#include <cstdint>
#include <vector>

enum class DeviceRole
{
    Unassigned,
    BottomLeft,
    BottomRight,
    TopLeft,
    TopRight
};

// Matches Physical Layout V1 Mode 1 / 2 / 3 and control_state.h naming.
enum class PerformanceMode
{
    SynthOnly,       // Mode 1 — instrument only
    VocoderOnly,     // Mode 2
    SynthAndVocoder  // Mode 3 — vocoder + dry voice path
};

struct KeyboardAssignment
{
    int device_index;
    DeviceRole role;
};

class InstrumentState
{
public:
    InstrumentState();

    void reset();

    // Keyboard assignment
    void assignKeyboard(int device_index, DeviceRole role);
    bool allKeyboardsAssigned() const;
    DeviceRole roleForKeyboard(int device_index) const;

    // Performance mode
    void setMode(PerformanceMode mode);
    PerformanceMode mode() const;

    // Global controls
    void setTranspose(int semitones);
    int transpose() const;
    void adjustTranspose(int delta);   // clamped reasonably

    void setMasterVolume(int value);
    int masterVolume() const;
    void adjustMasterVolume(int delta);

    // Head-tilt octave
    void setOctave(int value);
    int octave() const;
    void adjustOctave(int delta);

    // Breath / nod
    void setBreath(int value);
    int breath() const;
    void setNod(int value);
    int nod() const;

    // Mixer (0-127). Actual PipeWire gains are Phase 4; these are the
    // values the upper-keyboard mixer row writes and that Phase 4 will read.
    void setDryMix(int value);
    int dryMix() const;
    void adjustDryMix(int delta);

    void setVocoderMix(int value);
    int vocoderMix() const;
    void adjustVocoderMix(int delta);

    void setDrumMix(int value);
    int drumMix() const;
    void adjustDrumMix(int delta);

    // One-shot flags consumed by Engine
    void requestPanic();
    bool consumePanic();

private:
    static int clamp127(int v);
    static int clampOctave(int v);

    std::vector<KeyboardAssignment> assignments_;

    PerformanceMode mode_;

    int transpose_;
    int master_volume_;
    int octave_;

    int breath_;
    int nod_;

    int dry_mix_;
    int vocoder_mix_;
    int drum_mix_;

    bool panic_requested_;
};
