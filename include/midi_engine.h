#pragma once

#include <string>
#include <map>
#include <vector>

#include <alsa/asoundlib.h>


class MidiEngine
{
public:

    MidiEngine();

    ~MidiEngine();



    bool initialize();


    void shutdown();



    bool sendNoteOn(
        int channel,
        int note,
        int velocity,
        int cents
    );



    bool sendNoteOff(
        int channel,
        int note
    );
    bool sendDrumNoteOn(int channel, int note, int velocity);
    bool sendDrumNoteOff(int channel, int note);



    bool sendControlChange(
        int channel,
        int controller,
        int value
    );



    bool sendPitchBend(
        int channel,
        int value
    );



    bool sendProgramChange(
        int channel,
        int program
    );



    bool setVolume(
        int volume
    );



    int outputPort() const;
    // [zynvol] breath level 0..1 -> zynvol (14-bit CC7/CC39 on port "MIDI Volume")
    bool sendBreathVolume(float x);
    void resendBreathVolume();



private:

    snd_seq_t* seq_;

    int port_;
    int port_drums_;
    int port_volume_ = -1;
    int last_breath_code_ = 0;



    struct ActiveNote
    {
        int note;

        int channel;
    };


    std::map<int, ActiveNote> active_notes_;



private:

    bool sendEvent(
        snd_seq_event_t& event
    );



    int centsToPitchBend(
        int cents
    ) const;
};
