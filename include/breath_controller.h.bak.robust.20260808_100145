#pragma once

#include <string>

#include <alsa/asoundlib.h>


class BreathController
{
public:

    BreathController();

    ~BreathController();



    bool initialize();


    void shutdown();



    bool isConnected() const;



    int breathValue() const;


    int nodValue() const;



    void update();



private:

    snd_seq_t* seq_;


    int client_id_;

    int port_id_;
int local_port_; // ALSA port WE create on our own client, so
// subscribeToController() has a real destination to
// subscribe to (root cause of the silent subscribe
// failure -- see fix_breath_controller.sh).



    int breath_value_;

    int nod_value_;



    bool connected_;



private:

    bool findController();


    bool subscribeToController(
        int client,
        int port
    );
};
