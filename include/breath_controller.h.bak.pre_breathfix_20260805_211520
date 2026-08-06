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
