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



private:

    snd_seq_t* seq_;

    int port_;



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
