#include "midi_engine.h"

#include "logger.h"

#include <cmath>



MidiEngine::MidiEngine()
:
seq_(nullptr),
port_(-1)
{
}



MidiEngine::~MidiEngine()
{
    shutdown();
}



bool MidiEngine::initialize()
{
    Logger::info("Initializing MIDI engine...");


    if (
        snd_seq_open(
            &seq_,
            "default",
            SND_SEQ_OPEN_OUTPUT,
            0
        ) < 0
    )
    {
        Logger::error(
            "Could not open ALSA sequencer."
        );

        return false;
    }



    snd_seq_set_client_name(
        seq_,
        "Instrument_3"
    );



    port_ =
        snd_seq_create_simple_port(
            seq_,
            "MIDI Output",
            SND_SEQ_PORT_CAP_READ |
            SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC
        );



    if (port_ < 0)
    {
        Logger::error(
            "Could not create MIDI port."
        );

        return false;
    }



    return true;
}



void MidiEngine::shutdown()
{
    if (seq_)
    {
        snd_seq_close(seq_);
        seq_ = nullptr;
    }


    Logger::info(
        "MIDI engine shut down."
    );
}



bool MidiEngine::sendEvent(
    snd_seq_event_t& event
)
{
    snd_seq_ev_set_source(
        &event,
        port_
    );

    snd_seq_ev_set_subs(
        &event
    );

    snd_seq_ev_set_direct(
        &event
    );


    return
        snd_seq_event_output_direct(
            seq_,
            &event
        ) >= 0;
}



int MidiEngine::centsToPitchBend(
    int cents
) const
{
    double value =
        (cents / 200.0) * 8192.0;


    if (value < -8192)
        value = -8192;


    if (value > 8191)
        value = 8191;


    return static_cast<int>(
        std::lround(value)
    );
}



bool MidiEngine::sendPitchBend(
    int channel,
    int value
)
{
    snd_seq_event_t event;

    snd_seq_ev_clear(&event);


    snd_seq_ev_set_pitchbend(
        &event,
        channel,
        value
    );


    return sendEvent(event);
}



bool MidiEngine::sendNoteOn(
    int channel,
    int note,
    int velocity,
    int cents
)
{
    if (!sendPitchBend(
        channel,
        centsToPitchBend(cents)
    ))
    {
        return false;
    }



    snd_seq_event_t event;

    snd_seq_ev_clear(&event);



    snd_seq_ev_set_noteon(
        &event,
        channel,
        note,
        velocity
    );



    if (!sendEvent(event))
        return false;



    int id =
        (channel << 8) |
        note;



    active_notes_[id] =
    {
        note,
        channel
    };



    return true;
}



bool MidiEngine::sendNoteOff(
    int channel,
    int note
)
{
    snd_seq_event_t event;

    snd_seq_ev_clear(&event);



    snd_seq_ev_set_noteoff(
        &event,
        channel,
        note,
        0
    );


    return sendEvent(event);
}



bool MidiEngine::sendControlChange(
    int channel,
    int controller,
    int value
)
{
    snd_seq_event_t event;

    snd_seq_ev_clear(&event);



    snd_seq_ev_set_controller(
        &event,
        channel,
        controller,
        value
    );


    return sendEvent(event);
}



bool MidiEngine::sendProgramChange(
    int channel,
    int program
)
{
    snd_seq_event_t event;

    snd_seq_ev_clear(&event);



    snd_seq_ev_set_pgmchange(
        &event,
        channel,
        program
    );


    return sendEvent(event);
}



bool MidiEngine::setVolume(
    int volume
)
{
    for (
        int channel = 0;
        channel < 16;
        channel++
    )
    {
        sendControlChange(
            channel,
            7,
            volume
        );
    }


    return true;
}



int MidiEngine::outputPort() const
{
    return port_;
}
