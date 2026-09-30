// zynvol -- tiny MIDI-controlled stereo volume (a "MIDI VCA") for PipeWire/JACK.
//
//   audio in  (2ch) --[ gain ]--> audio out (2ch)        JACK client "zyn-volume-node"
//   gain is set by MIDI from the instrument engine       ALSA sequencer port "zynvol:in"
//
// MIDI protocol (channel 0): CC 7 = MSB, CC 39 = LSB of a 14-bit value (0..16383).
// x = value/16383 is the instrument's breath level (0..1). gain = x^exponent.
// Default exponent 3.0 matches `pactl set-sink-volume N%` (PipeWire-Pulse applies a
// cubic law), i.e. the same response the old pw-loopback volume node had.
// Until the first MIDI message arrives the tool is a transparent pass-through.
#include <alsa/asoundlib.h>
#include <jack/jack.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::atomic<bool> g_run{true};
static std::atomic<float> g_target{1.0f};
static float g_cur = 1.0f;       // audio-thread only
static float g_coef = 0.01f;     // ~4 ms one-pole smoothing (anti-zipper)
static jack_port_t *g_in[2], *g_out[2];

static void on_signal(int) { g_run = false; }
static void on_jack_shutdown(void*) { g_run = false; }

static int process(jack_nframes_t n, void*) {
    const float tgt = g_target.load(std::memory_order_relaxed);
    float* in[2]  = {(float*)jack_port_get_buffer(g_in[0], n),  (float*)jack_port_get_buffer(g_in[1], n)};
    float* out[2] = {(float*)jack_port_get_buffer(g_out[0], n), (float*)jack_port_get_buffer(g_out[1], n)};
    float g = g_cur;
    for (jack_nframes_t i = 0; i < n; ++i) {
        g += (tgt - g) * g_coef;
        if (std::fabs(tgt - g) < 1e-6f) g = tgt;   // exact 0 stays exactly 0
        out[0][i] = in[0][i] * g;
        out[1][i] = in[1][i] * g;
    }
    g_cur = g;
    return 0;
}

// Find "<client_name>" / "<port_name>" in the ALSA sequencer and subscribe to it.
static bool connect_to_engine(snd_seq_t* seq, int my_port, const char* cname, const char* pname) {
    snd_seq_client_info_t* ci; snd_seq_port_info_t* pi;
    snd_seq_client_info_alloca(&ci); snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0) {
        if (std::strcmp(snd_seq_client_info_get_name(ci), cname) != 0) continue;
        const int cl = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, cl);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(seq, pi) >= 0) {
            if (std::strcmp(snd_seq_port_info_get_name(pi), pname) != 0) continue;
            const int r = snd_seq_connect_from(seq, my_port, cl, snd_seq_port_info_get_port(pi));
            return r >= 0 || r == -EEXIST;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    std::string name = "zyn-volume-node", mclient = "Instrument_3", mport = "MIDI Volume";
    float expo = 3.0f;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--name")) name = argv[i + 1];
        else if (!std::strcmp(argv[i], "--exponent")) expo = (float)std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--midi-client")) mclient = argv[i + 1];
        else if (!std::strcmp(argv[i], "--midi-port")) mport = argv[i + 1];
    }
    prctl(PR_SET_PDEATHSIG, SIGTERM);            // die with the instrument
    std::signal(SIGTERM, on_signal); std::signal(SIGINT, on_signal);

    jack_status_t st;
    jack_client_t* jc = jack_client_open(name.c_str(), JackNoStartServer, &st);
    if (!jc) { std::fprintf(stderr, "zynvol: cannot open JACK/PipeWire client\n"); return 2; }
    g_in[0]  = jack_port_register(jc, "in_1",  JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput,  0);
    g_in[1]  = jack_port_register(jc, "in_2",  JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput,  0);
    g_out[0] = jack_port_register(jc, "out_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_out[1] = jack_port_register(jc, "out_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    g_coef = 1.0f - std::exp(-1.0f / (0.004f * (float)jack_get_sample_rate(jc)));
    jack_set_process_callback(jc, process, nullptr);
    jack_on_shutdown(jc, on_jack_shutdown, nullptr);
    if (jack_activate(jc)) { std::fprintf(stderr, "zynvol: activate failed\n"); return 2; }

    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
        std::fprintf(stderr, "zynvol: cannot open ALSA sequencer (staying pass-through)\n");
        while (g_run) sleep(1);
        jack_client_close(jc); return 0;
    }
    snd_seq_set_client_name(seq, "zynvol");
    const int port = snd_seq_create_simple_port(seq, "in",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

    const int npfd = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd* pfd = (struct pollfd*)std::calloc((size_t)npfd, sizeof(struct pollfd));
    snd_seq_poll_descriptors(seq, pfd, (unsigned)npfd, POLLIN);

    int msb = 0, lsb = 0, since_scan = 1000;
    while (g_run) {
        if (++since_scan >= 4) {                       // ~every 2 s: (re)subscribe to the engine
            since_scan = 0;
            connect_to_engine(seq, port, mclient.c_str(), mport.c_str());
        }
        if (poll(pfd, (nfds_t)npfd, 500) <= 0) continue;
        snd_seq_event_t* ev = nullptr;
        while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
            if (ev->type == SND_SEQ_EVENT_CONTROLLER) {
                const unsigned p = ev->data.control.param;
                const int v = (int)(ev->data.control.value & 127);
                if (p == 39) { lsb = v; }
                else if (p == 7) { msb = v; } else continue;
                const float x = (float)((msb << 7) | lsb) / 16383.0f;
                g_target.store(std::pow(x, expo), std::memory_order_relaxed);
            }
        }
    }
    snd_seq_close(seq);
    jack_client_close(jc);
    std::free(pfd);
    return 0;
}
