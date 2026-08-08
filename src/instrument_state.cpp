#include "instrument_state.h"

InstrumentState::InstrumentState()
{
    reset();
}

void InstrumentState::reset()
{
    assignments_.clear();

    mode_ = PerformanceMode::SynthOnly;

    transpose_ = 0;
    master_volume_ = 127;
    octave_ = 5;   // matches Instrument_2 initial tilt_octave

    breath_ = 0;
    nod_ = 64;

    dry_mix_ = 100;
    vocoder_mix_ = 100;
    drum_mix_ = 100;
 synth_mix_ = 100;
 mic_mix_ = 100;

    panic_requested_ = false;
}

int InstrumentState::clamp127(int v)
{
    if (v < 0) return 0;
    if (v > 127) return 127;
    return v;
}

int InstrumentState::clampOctave(int v)
{
    if (v < 0) return 0;
    if (v > 7) return 7;
    return v;
}

// ================= Keyboard assignment =================

void InstrumentState::assignKeyboard(int device_index, DeviceRole role)
{
    for (const auto& assignment : assignments_)
    {
        if (assignment.device_index == device_index)
            return;
    }

    assignments_.push_back({device_index, role});
}

bool InstrumentState::allKeyboardsAssigned() const
{
    return assignments_.size() == 4;
}

DeviceRole InstrumentState::roleForKeyboard(int device_index) const
{
    for (const auto& assignment : assignments_)
    {
        if (assignment.device_index == device_index)
            return assignment.role;
    }
    return DeviceRole::Unassigned;
}

// ================= Performance mode =================

void InstrumentState::setMode(PerformanceMode mode)
{
    mode_ = mode;
}

PerformanceMode InstrumentState::mode() const
{
    return mode_;
}

// ================= Global controls =================

void InstrumentState::setTranspose(int semitones)
{
    // Keep within a musical working range
    if (semitones < -24) semitones = -24;
    if (semitones > 24)  semitones = 24;
    transpose_ = semitones;
}

int InstrumentState::transpose() const
{
    return transpose_;
}

void InstrumentState::adjustTranspose(int delta)
{
    setTranspose(transpose_ + delta);
}

void InstrumentState::setMasterVolume(int value)
{
    master_volume_ = clamp127(value);
}

int InstrumentState::masterVolume() const
{
    return master_volume_;
}

void InstrumentState::adjustMasterVolume(int delta)
{
    setMasterVolume(master_volume_ + delta);
}

void InstrumentState::setOctave(int value)
{
    octave_ = clampOctave(value);
}

int InstrumentState::octave() const
{
    return octave_;
}

void InstrumentState::adjustOctave(int delta)
{
    setOctave(octave_ + delta);
}

// ================= Breath / nod =================

void InstrumentState::setBreath(int value)
{
    breath_ = clamp127(value);
}

int InstrumentState::breath() const
{
    return breath_;
}

void InstrumentState::setNod(int value)
{
    nod_ = clamp127(value);
}

int InstrumentState::nod() const
{
    return nod_;
}

// ================= Mixer =================

void InstrumentState::setDryMix(int value)      { dry_mix_ = clamp127(value); }
int  InstrumentState::dryMix() const            { return dry_mix_; }
void InstrumentState::adjustDryMix(int delta)   { setDryMix(dry_mix_ + delta); }

void InstrumentState::setVocoderMix(int value)     { vocoder_mix_ = clamp127(value); }
int  InstrumentState::vocoderMix() const           { return vocoder_mix_; }
void InstrumentState::adjustVocoderMix(int delta)  { setVocoderMix(vocoder_mix_ + delta); }

void InstrumentState::setDrumMix(int value)     { drum_mix_ = clamp127(value); }
int  InstrumentState::drumMix() const           { return drum_mix_; }
void InstrumentState::adjustDrumMix(int delta)  { setDrumMix(drum_mix_ + delta); }

void InstrumentState::setSynthMix(int value) { synth_mix_ = clamp127(value); }
int InstrumentState::synthMix() const { return synth_mix_; }
void InstrumentState::adjustSynthMix(int delta) { setSynthMix(synth_mix_ + delta); }

void InstrumentState::setMicMix(int value) { mic_mix_ = clamp127(value); }
int InstrumentState::micMix() const { return mic_mix_; }
void InstrumentState::adjustMicMix(int delta) { setMicMix(mic_mix_ + delta); }


// ================= Panic =================

void InstrumentState::requestPanic()
{
    panic_requested_ = true;
}

bool InstrumentState::consumePanic()
{
    if (!panic_requested_)
        return false;
    panic_requested_ = false;
    return true;
}
