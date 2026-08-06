#include "breath_controller.h"
#include <algorithm>
#include <cctype>

#include "logger.h"



BreathController::BreathController()
:
seq_(nullptr),
client_id_(-1),
port_id_(-1),
local_port_(-1),
breath_value_(0),
nod_value_(64),
connected_(false)
{
}



BreathController::~BreathController()
{
    shutdown();
}



bool BreathController::initialize()
{
    Logger::info(
        "Initializing breath controller..."
    );


        // FIX: close any handle from a previous failed attempt first -- without this,
    // every retry in StartupManager::verifyBreathController()'s poll loop
    // leaked a new ALSA sequencer client. After enough retries (every
    // 500ms) this exhausts the ALSA seq client table and snd_seq_open()
    // itself starts failing, so the controller can never be found no
    // matter how long it's left plugged in -- confirmed in instrument.log.
    if (seq_)
    {
        snd_seq_close(seq_);
        seq_ = nullptr;
        connected_ = false;
    }
    if (
        snd_seq_open(
            &seq_,
            "default",
            SND_SEQ_OPEN_DUPLEX,
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
        "Instrument_3_Breath"
    );


    // FIX (Phase 0): without this, snd_seq_event_input() in update()
    // blocks the calling thread whenever there's no pending event --
    // which would freeze Engine::run()'s entire main loop (keyboards,
    // MIDI, everything) any time breath data isn't actively streaming.
    snd_seq_nonblock(
        seq_,
        1
    );
// FIX: subscribeToController() below subscribes a remote source to
// "destination.port" on OUR client -- but without creating a port
// here, no such port exists, so that subscribe call fails silently
// every time regardless of whether the breath controller was
// correctly found (confirmed root cause of the "detected but not
// found" symptom -- see fix_breath_controller.sh).
local_port_ = snd_seq_create_simple_port(
    seq_,
    "breath_in",
    SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
    SND_SEQ_PORT_TYPE_APPLICATION
);
if (local_port_ < 0)
{
    Logger::error("Could not create ALSA sequencer port for breath controller.");
    return false;
}



    connected_ =
        findController();



    if (!connected_)
    {
        Logger::error(
            "Breath controller not found."
        );
    }



    return connected_;
}



void BreathController::shutdown()
{
    if (seq_)
    {
        snd_seq_close(seq_);
        seq_ = nullptr;
    }


    connected_ = false;
}



bool BreathController::findController()
{
    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t* pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);

    // Case-insensitive containment check.
    auto containsCi = [](const std::string& haystack, const std::string& needle)
    {
        auto it = std::search(
            haystack.begin(), haystack.end(),
            needle.begin(), needle.end(),
            [](unsigned char a, unsigned char b)
            {
                return std::tolower(a) == std::tolower(b);
            }
        );
        return it != haystack.end();
    };

    while (snd_seq_query_next_client(seq_, cinfo) >= 0)
    {
        int client = snd_seq_client_info_get_client(cinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq_, pinfo) >= 0)
        {
            std::string name = snd_seq_port_info_get_name(pinfo);
            // Logged unconditionally so a non-match is self-diagnosing
            // from instrument.log -- if this device is still not found,
            // the exact port name ALSA reports will be right here.
            Logger::info("  ALSA seq port seen: \"" + name + "\"");
            if (containsCi(name, "breath controller") || containsCi(name, "tecontrol"))
            {
                return subscribeToController(client, snd_seq_port_info_get_port(pinfo));
            }
        }
    }
    return false;
}



bool BreathController::subscribeToController(
    int client,
    int port
)
{
    snd_seq_port_subscribe_t* subs;


    snd_seq_port_subscribe_alloca(
        &subs
    );



    snd_seq_addr_t sender;

    sender.client = client;
    sender.port = port;



    snd_seq_addr_t destination;

    destination.client =
        snd_seq_client_id(seq_);

    destination.port = local_port_;



    snd_seq_port_subscribe_set_sender(
        subs,
        &sender
    );


    snd_seq_port_subscribe_set_dest(
        subs,
        &destination
    );



    if (
        snd_seq_subscribe_port(
            seq_,
            subs
        ) < 0
    )
    {
        return false;
    }



    client_id_ = client;
    port_id_ = port;



    Logger::info(
        "Breath controller connected."
    );


    return true;
}



void BreathController::update()
{
    if (!seq_)
        return;



    snd_seq_event_t* event;



    while (
        snd_seq_event_input(
            seq_,
            &event
        ) >= 0
    )
    {

        if (
            event->type ==
            SND_SEQ_EVENT_CONTROLLER
        )
        {

            int controller =
                event->data.control.param;


            int value =
                event->data.control.value;



            if (controller == 2)
            {
                breath_value_ = value;
            }


            else if (controller == 12)
            {
                nod_value_ = value;
            }
        }



        snd_seq_free_event(event);


        if (
            snd_seq_event_input_pending(
                seq_,
                0
            ) == 0
        )
        {
            break;
        }
    }
}



bool BreathController::isConnected() const
{
    return connected_;
}



int BreathController::breathValue() const
{
    return breath_value_;
}



int BreathController::nodValue() const
{
    return nod_value_;
}
