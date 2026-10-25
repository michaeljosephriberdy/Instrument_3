#include "audio_graph_manager.h"
#include <fcntl.h>
#include <unistd.h>
#include <regex>
#include <map>
#include <cmath>
#include <atomic>
#include "logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <thread>

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PI_AUDIO_LEFT_SINK
#define PI_AUDIO_LEFT_SINK "alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FL"
#endif
#ifndef PI_AUDIO_RIGHT_SINK
#define PI_AUDIO_RIGHT_SINK "alsa_output.pci-0000_04_00.6.HiFi__Headphones__sink:playback_FR"
#endif


namespace
{
    std::string which(const std::string& name)
    {
        if (name.empty())
            return {};
        // Absolute path?
        if (name.front() == '/')
            return name;

        std::string cmd = "command -v " + name + " 2>/dev/null";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f)
            return {};
        char buf[512];
        std::string out;
        if (fgets(buf, sizeof(buf), f))
        {
            out = buf;
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
                out.pop_back();
        }
        pclose(f);
        return out;
    }

    std::string shellCapture(const std::string& cmd)
    {
        FILE* f = popen(cmd.c_str(), "r");
        if (!f)
            return {};
        std::string out;
        char buf[1024];
        while (fgets(buf, sizeof(buf), f))
            out += buf;
        pclose(f);
        return out;
    }

    // First non-HDMI playback sink port matching left/right preference.
    std::string defaultPlaybackPort(bool left)
{
    // Resolve playback ports via shell. Configured sink first, then Headphones; never HDMI.
    const char* flfr = left ? "playback_FL" : "playback_FR";
    // [fix_all] configured-sink-first: the sink chosen at build time (laptop
    // headphones / Pi Shure) wins whenever it exists in the graph.
    {
        const std::string want_port = left ? std::string(PI_AUDIO_LEFT_SINK)
                                           : std::string(PI_AUDIO_RIGHT_SINK);
        std::string hit = shellCapture(std::string("pw-link -i 2>/dev/null | grep -F '") +
                                       want_port + "' | head -n 1");
        while (!hit.empty() && (hit.back() == '\n' || hit.back() == '\r' || hit.back() == ' '))
            hit.pop_back();
        if (!hit.empty())
            return hit;
    }
    // Try Headphones first.
    std::string cmd =
        std::string("pw-link -i 2>/dev/null | grep -i '") + flfr +
        "' | grep -vi hdmi | "
        "grep -i headphone | head -n 1";
    std::string line = shellCapture(cmd);
    if (line.empty()) {
        // Any non-HDMI sink.
        cmd =
            std::string("pw-link -i 2>/dev/null | grep -i '") + flfr +
            "' | grep -vi hdmi | head -n 1";
        line = shellCapture(cmd);
    }
    if (line.empty()) {
        // Last resort: any matching playback port.
        cmd =
            std::string("pw-link -i 2>/dev/null | grep -i '") + flfr +
            "' | grep -vi hdmi | head -n 1";
        line = shellCapture(cmd);
    }
    while (!line.empty()) {
        const int b = static_cast<unsigned char>(line.back());
        if (b == 10 || b == 13 || b == 32)
            line.pop_back();
        else
            break;
    }
    return line;
}
}

AudioGraphManager::AudioGraphManager() = default;

AudioGraphManager::~AudioGraphManager()
{
    stopProcesses();
}

void AudioGraphManager::setConfig(const Config& cfg)
{
    cfg_ = cfg;
}

bool AudioGraphManager::processAlive(pid_t pid) const {
    if (pid <= 0) {
        return false;
    }
    // kill(pid, 0) succeeds for a zombie too -- the PID stays valid in
    // the process table until the parent reaps it with waitpid(). Reap
    // first so a child that crashed on its own (segfault, killed by a
    // PipeWire/wireplumber restart, etc.) actually reads as dead. That
    // lets ensureHealthyGraph() and launchSooperLooper()/launchZyn()/
    // launchZynDrums()/launchVocoder() detect the crash and relaunch.
    int status = 0;
    pid_t reaped = waitpid(pid, &status, WNOHANG);
    if (reaped == pid) {
        return false;
    }
    return kill(pid, 0) == 0;
}



bool AudioGraphManager::zynRunning() const
{
    return processAlive(zyn_pid_);
}

bool AudioGraphManager::sooperlooperRunning() const
{
    return processAlive(sl_pid_);
}

bool AudioGraphManager::vocoderRunning() const
{
    return processAlive(voc_pid_);
}

void AudioGraphManager::killPid(pid_t& pid, const char* label)
{
    if (pid <= 0)
        return;

    if (kill(pid, 0) == 0)
    {
        Logger::info(std::string("Stopping ") + label +
                     " (pid " + std::to_string(pid) + ")");
        kill(pid, SIGTERM);
        // Brief wait, then escalate.
        for (int i = 0; i < 20; ++i)
        {
            if (waitpid(pid, nullptr, WNOHANG) > 0)
            {
                pid = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }
    pid = -1;
}

bool AudioGraphManager::pwLink(const std::string& src, const std::string& dst)
{
    if (src.empty() || dst.empty())
        return false;

    // Port names can contain spaces (e.g. the ALSA-MIDI bridge names
    // ports like "Midi-Bridge:Instrument_3MIDI Output"). Quote both
    // so the shell doesn't split them into extra argv entries -- an
    // unquoted space here silently turned one port name into two
    // arguments, which is why previously-"correct" names still failed.
    std::string cmd = "pw-link '" + src + "' '" + dst + "' 2>&1";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f)
    {
        Logger::warning("pw-link: failed to launch subprocess for " +
            src + " → " + dst);
        return false;
    }
    std::string output;
    char buf[512];
    while (fgets(buf, sizeof(buf), f))
        output += buf;
    int status = pclose(f);
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    if (status == 0)
    {
        owned_links_.push_back({src, dst});
        Logger::info("pw-link: " + src + " → " + dst);
        return true;
    }
    Logger::warning("pw-link failed: " + src + " → " + dst +
        (output.empty() ? "" : (" -- " + output)));
    return false;
}

bool AudioGraphManager::pwUnlink(const std::string& src, const std::string& dst)
{
    if (src.empty() || dst.empty())
        return false;
    // Quote exactly like pwLink — names can contain spaces.
    std::string cmd = "pw-link -d '" + src + "' '" + dst + "' 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

void AudioGraphManager::disconnectAllOwnedLinks()
{
    for (auto it = owned_links_.rbegin(); it != owned_links_.rend(); ++it)
    {
        pwUnlink(it->first, it->second);
    }
    owned_links_.clear();
}

std::vector<std::string>
AudioGraphManager::findPorts(const std::string& substring, bool inputs) const
{
    std::vector<std::string> result;
    std::string flag = inputs ? "-i" : "-o";
    std::string cmd = "pw-link " + flag + " 2>/dev/null";
    std::string out = shellCapture(cmd);
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.find(substring) != std::string::npos)
            result.push_back(line);
    }
    return result;
}


std::string AudioGraphManager::findEngineMidiPort() const
{
    // Melodic port from MidiEngine: "MIDI Output"
    auto ports = findPortsMatching(
        {"Instrument_3MIDI Output", "Instrument_3:MIDI Output",
         "MIDI Output (capture)"},
        false);
    // Prefer lines that look like melodic, not Drums
    for (const auto& p : ports) {
        std::string low = p;
        for (char& c : low)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.find("drum") != std::string::npos)
            continue;
        if (low.find("midi output") != std::string::npos ||
            low.find("midi_output") != std::string::npos)
            return p;
    }
    if (!ports.empty())
        return ports[0];
    // Fallback common PipeWire bridge name
    return "Midi-Bridge:Instrument_3MIDI Output (capture)";
}

std::string AudioGraphManager::findEngineDrumsMidiPort() const
{
    auto ports = findPortsMatching(
        {"Instrument_3MIDI Drums", "MIDI Drums", "Instrument_3:MIDI Drums"},
        false);
    for (const auto& p : ports) {
        std::string low = p;
        for (char& c : low)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.find("drum") != std::string::npos)
            return p;
    }
    if (!ports.empty())
        return ports[0];
    return "Midi-Bridge:Instrument_3MIDI Drums (capture)";
}




bool AudioGraphManager::micPresent() const
{
    return queryMic().present;
}



bool AudioGraphManager::launchZyn()
{
    if (processAlive(zyn_pid_))
        return true;
    // Runtime identity of the melodic instance.
    if (cfg_.zyn_client_name.empty() || cfg_.zyn_client_name == "zynaddsubfx")
        cfg_.zyn_client_name = "zyn-melody";

    std::string bin = which(cfg_.zyn_binary.empty() ? "zynaddsubfx" : cfg_.zyn_binary);
    if (bin.empty()) {
        Logger::error("zynaddsubfx not found");
        return false;
    }
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        std::string pwjack = which("pw-jack");
        const char* name = cfg_.zyn_client_name.c_str();
        // [instrument-bus] never let PipeWire auto-link this client to the default sink
        setenv("PIPEWIRE_PROPS", "{ node.autoconnect=false }", 1);
        if (!pwjack.empty())
            execlp("pw-jack", "pw-jack", bin.c_str(),
                   "-U", "-N", name, "-l", cfg_.zyn_instrument.c_str(),
                   "-I", "jack", "-O", "jack", "-a",
                   static_cast<char*>(nullptr));
        execlp(bin.c_str(), bin.c_str(),
               "-U", "-N", name, "-l", cfg_.zyn_instrument.c_str(),
               "-I", "jack", "-O", "jack", "-a",
               static_cast<char*>(nullptr));
        _exit(127);
    }
    zyn_pid_ = pid;
    Logger::info("Launched MELODY Zyn pid=" + std::to_string(pid) +
                 " client='" + cfg_.zyn_client_name +
                 "' file=" + cfg_.zyn_instrument);
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!findPortsMatching({cfg_.zyn_client_name}, false).empty()) {
            Logger::info("Melody Zyn ports visible");
    // PipeWire often auto-links new JACK clients to the default sink.
    // Kill that immediately so Mode graphs own the routing.
    {
        std::string play_l = defaultPlaybackPort(true);
        std::string play_r = defaultPlaybackPort(false);
        if (play_l.empty())
            play_l = PI_AUDIO_LEFT_SINK;
        if (play_r.empty())
            play_r = PI_AUDIO_RIGHT_SINK;
        std::string ol = resolveClientPort(cfg_.zyn_client_name, "out_1", false);
        std::string or_ = resolveClientPort(cfg_.zyn_client_name, "out_2", false);
        if (ol.empty()) ol = "zynaddsubfx_zyn-melody:out_1";
        if (or_.empty()) or_ = "zynaddsubfx_zyn-melody:out_2";
        pwUnlink(ol, play_l); pwUnlink(or_, play_r);
    }
            break;
        }
    }
    return true;
}


bool AudioGraphManager::launchZynDrums()
{
    if (processAlive(zyn_drums_pid_))
        return true;
    if (cfg_.zyn_drums_client_name.empty())
        cfg_.zyn_drums_client_name = "zyn-drums";
    if (cfg_.zyn_drums_instrument.empty())
        cfg_.zyn_drums_instrument = "config/drums.xmz";

    std::string bin = which(cfg_.zyn_binary.empty() ? "zynaddsubfx" : cfg_.zyn_binary);
    if (bin.empty())
        return false;
    {
        std::ifstream f(cfg_.zyn_drums_instrument);
        if (!f.good()) {
            Logger::warning("drums file missing: " + cfg_.zyn_drums_instrument);
            return false;
        }
    }
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        std::string pwjack = which("pw-jack");
        const char* name = cfg_.zyn_drums_client_name.c_str();
        // [instrument-bus] never let PipeWire auto-link this client to the default sink
        setenv("PIPEWIRE_PROPS", "{ node.autoconnect=false }", 1);
        if (!pwjack.empty())
            execlp("pw-jack", "pw-jack", bin.c_str(),
                   "-U", "-N", name, "-l", cfg_.zyn_drums_instrument.c_str(),
                   "-I", "jack", "-O", "jack", "-a",
                   static_cast<char*>(nullptr));
        execlp(bin.c_str(), bin.c_str(),
               "-U", "-N", name, "-l", cfg_.zyn_drums_instrument.c_str(),
               "-I", "jack", "-O", "jack", "-a",
               static_cast<char*>(nullptr));
        _exit(127);
    }
    zyn_drums_pid_ = pid;
    Logger::info("Launched DRUMS Zyn pid=" + std::to_string(pid) +
                 " client='" + cfg_.zyn_drums_client_name +
                 "' file=" + cfg_.zyn_drums_instrument);
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!findPortsMatching({cfg_.zyn_drums_client_name}, false).empty()) {
            Logger::info("Drums Zyn ports visible");
    {
        std::string play_l = defaultPlaybackPort(true);
        std::string play_r = defaultPlaybackPort(false);
        if (play_l.empty())
            play_l = PI_AUDIO_LEFT_SINK;
        if (play_r.empty())
            play_r = PI_AUDIO_RIGHT_SINK;
        std::string ol = resolveClientPort(cfg_.zyn_drums_client_name, "out_1", false);
        std::string or_ = resolveClientPort(cfg_.zyn_drums_client_name, "out_2", false);
        if (ol.empty()) ol = "zynaddsubfx_zyn-drums:out_1";
        if (or_.empty()) or_ = "zynaddsubfx_zyn-drums:out_2";
        pwUnlink(ol, play_l); pwUnlink(or_, play_r);
    }
            break;
        }
    }
    return true;
}





bool AudioGraphManager::recoverAudioStack(const char* reason)
{
    // Cooldown: never restart PipeWire more than once per 90 s.
    static std::chrono::steady_clock::time_point last_recovery{};
    auto now = std::chrono::steady_clock::now();
    if (last_recovery.time_since_epoch().count() != 0 &&
        (now - last_recovery) < std::chrono::seconds(90)) {
        Logger::warning(std::string("Audio recovery suppressed (90s cooldown): ") +
                        (reason ? reason : ""));
        return false;
    }
    last_recovery = now;

    Logger::warning(std::string("AUDIO RECOVERY: ") +
                    (reason ? reason : "unspecified") +
                    " -- stopping clients, restarting user PipeWire, relaunching");

    // Stop our children cleanly so we do not leave zombies across the
    // PipeWire restart.
    killPid(sl_pid_, "sooperlooper");
    killPid(voc_pid_, "vocoder");
    killPid(zyn_drums_pid_, "zyn-drums");
    killPid(zyn_pid_, "zyn-melody");
    killPid(bus_pid_, "instrument-bus"); // [instrument-bus]
    sl_pid_ = -1;
    voc_pid_ = -1;
    zyn_drums_pid_ = -1;
    zyn_pid_ = -1;

    // User-session restart — works headless, no root required.
    // pipewire-pulse + wireplumber keep the session coherent.
    int rc = std::system("systemctl --user restart pipewire pipewire-pulse wireplumber >/dev/null 2>&1");
    if (rc != 0) {
        Logger::error("systemctl --user restart pipewire... failed (rc=" +
                      std::to_string(rc) + ")");
        // Still try to bring our clients back on the existing session.
    } else {
        Logger::info("User PipeWire stack restarted");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    // Relaunch in a safe order: SL first (stable when it owns the graph
    // before the Zyns register), then the rest via normal start path.
    // [instrument-bus] relaunch the bus BEFORE Zyn / SooperLooper / the rest
    last_bus_ready_ = false;
    (void)launchBusNode();
    invalidatePortCache();
    sweep_pending_ = true;
    bool ok = launchSooperLooper();
    ok = launchZyn() && ok;
    ok = launchZynDrums() && ok;
    // Vocoder is mode-dependent; ensureHealthyGraph / setMode will start it
    // when needed. Still try once so Mode 2/3 recover quickly.
    (void)launchVocoder();

    // Rebuild links for the current mode.
    if (!setMode(mode_)) {
        Logger::warning("recoverAudioStack: setMode returned false");
        ok = false;
    }

    if (ok) {
        Logger::info("AUDIO RECOVERY complete — graph rebuilt for current mode");
    } else {
        Logger::warning("AUDIO RECOVERY finished with some failures "
                        "(see earlier warnings; health will keep trying)");
    }
    return ok;
}

bool AudioGraphManager::launchSooperLooper()
{
    if (processAlive(sl_pid_))
        return true;

    std::string bin = which(cfg_.sooperlooper_binary.empty()
                            ? "sooperlooper"
                            : cfg_.sooperlooper_binary);
    if (bin.empty()) {
        Logger::warning("sooperlooper not found -- install package 'sooperlooper'; "
                        "loop keys will log only");
        return false;
    }

    // Rate-limit relaunches so we never recreate the restart storm that
    // corrupts PipeWire/JACK. First launch of a session is never delayed.
    static std::chrono::steady_clock::time_point last_launch{};
    auto now = std::chrono::steady_clock::now();
    if (last_launch.time_since_epoch().count() != 0 &&
        (now - last_launch) < std::chrono::seconds(12)) {
        Logger::warning("SooperLooper relaunch suppressed (rate limit 12s) "
                        "-- required in every mode; will retry next health tick");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        Logger::error("fork() failed for SooperLooper");
        return false;
    }
    if (pid == 0) {
        setsid();
        int fd = open("/tmp/sooperlooper_engine.log",
                      O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        std::string port = std::to_string(cfg_.sooperlooper_osc_port);
        std::string pwjack = which("pw-jack");
        if (!pwjack.empty()) {
            execlp("pw-jack", "pw-jack",
                   bin.c_str(),
                   "-p", port.c_str(),
                   "-l", "4",
                   "-c", "2",
                   static_cast<char*>(nullptr));
        }
        execlp(bin.c_str(), bin.c_str(),
               "-p", port.c_str(),
               "-l", "4",
               "-c", "2",
               static_cast<char*>(nullptr));
        _exit(127);
    }

    sl_pid_ = pid;
    last_launch = now;
    Logger::info("Launched SooperLooper pid=" + std::to_string(pid) +
                 " OSC :" + std::to_string(cfg_.sooperlooper_osc_port) +
                 " (log: /tmp/sooperlooper_engine.log)");

    // Hard settle: process must stay alive AND ports must appear.
    bool ports_ok = false;
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!processAlive(sl_pid_)) {
            Logger::warning("SooperLooper exited during settle "
                            "(see /tmp/sooperlooper_engine.log)");
            sl_pid_ = -1;
            return false;
        }
        auto outs = findPortsMatching(
            {"sooperlooper", "SooperLooper", "common_out"}, false);
        if (!outs.empty()) {
            ports_ok = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            if (!processAlive(sl_pid_)) {
                Logger::warning("SooperLooper died right after ports appeared "
                                "(see /tmp/sooperlooper_engine.log)");
                sl_pid_ = -1;
                return false;
            }
            break;
        }
    }
    if (!ports_ok) {
        Logger::warning("SooperLooper ports never appeared (pid still alive)");
    } else {
        Logger::info("SL ports settled");
    }
    return processAlive(sl_pid_);
}



bool AudioGraphManager::launchVocoder()
{
    if (processAlive(voc_pid_))
        return true;

    // Prefer headless jalv; fall back to jalv.gtk.
    std::string jalv = which(cfg_.jalv_binary.empty() ? "jalv" : cfg_.jalv_binary);
    if (jalv.empty())
        jalv = which("jalv.gtk");
    if (jalv.empty())
        jalv = which("jalv.gtk3");
    if (jalv.empty())
    {
        Logger::warning("jalv not found — install 'jalv' and 'calf-plugins' for Mode 2/3");
        return false;
    }

    // Verify the LV2 URI is installed (lv2ls).
    std::string lv2ls = which("lv2ls");
    if (!lv2ls.empty())
    {
        std::string listed = shellCapture(lv2ls + " 2>/dev/null");
        if (listed.find("Vocoder") == std::string::npos &&
            listed.find(cfg_.calf_vocoder_uri) == std::string::npos)
        {
            Logger::warning("Calf Vocoder LV2 URI not found in lv2ls — "
                            "is calf-plugins installed?");
            // Still try; some systems omit URI strings from lv2ls output format.
        }
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        Logger::error("fork() failed for vocoder host");
        return false;
    }

    if (pid == 0) {
        // Log jalv stdout/stderr so Mode 2/3 failures are diagnosable.
        (void)freopen("/tmp/instrument_vocoder.log", "w", stdout);
        (void)freopen("/tmp/instrument_vocoder.log", "a", stderr);

        // VOCODER_STDIN_KEEPALIVE
        // jalv reads an interactive command stream from stdin and exits
        // cleanly (status 0) on EOF. Under systemd the forked child has
        // stdin already closed/at EOF, so jalv prints control ports and
        // quits within milliseconds -- the headless "Vocoder host exited
        // immediately" loop. Hold a pipe write end open across exec so
        // stdin never EOFs.
        int keep_stdin[2] = {-1, -1};
        if (pipe(keep_stdin) == 0) {
            (void)dup2(keep_stdin[0], STDIN_FILENO);
            (void)close(keep_stdin[0]);
            // keep_stdin[1] intentionally left open (no FD_CLOEXEC)
        }

        {
            std::string pwjack = which("pw-jack");
            if (!pwjack.empty()) {
                execlp("pw-jack", "pw-jack",
                       jalv.c_str(),
                       "-n", "InstrumentVocoder",
                       "-l", "config/calfvocoderpreset/state.ttl",
                       cfg_.calf_vocoder_uri.c_str(),
                       static_cast<char*>(nullptr));
            }
            execlp(jalv.c_str(), jalv.c_str(),
                   "-n", "InstrumentVocoder",
                   "-l", "config/calfvocoderpreset/state.ttl",
                   cfg_.calf_vocoder_uri.c_str(),
                   static_cast<char*>(nullptr));
        }
        _exit(127);
    }

    voc_pid_ = pid;
    Logger::info("Launched Calf Vocoder host pid=" + std::to_string(pid) +
                 " node=InstrumentVocoder");
                 // [vocoder-diag] A plugin-load failure (e.g. wrong LV2 URI) makes
                 // jalv exit within milliseconds. Catch that immediately
                 // instead of only noticing later that no ports ever showed up.
                 std::this_thread::sleep_for(std::chrono::milliseconds(300));
                 if (!processAlive(voc_pid_)) {
                     int voc_status = 0;
                     waitpid(voc_pid_, &voc_status, WNOHANG);
                     std::string voc_tail = shellCapture("tail -n 20 /tmp/instrument_vocoder.log 2>/dev/null");
                     Logger::error("Vocoder host exited immediately after launch (pid=" +
                         std::to_string(voc_pid_) + "). This is why Mode 2/3 fall back to " +
                         "un-vocoded audio. Check jalv and the Calf Vocoder LV2 URI ('" +
                         cfg_.calf_vocoder_uri + "'). Run diag_mode23_preflight.sh for a " +
                         "full check. Last vocoder output:\n" + voc_tail);
                     voc_pid_ = -1;
                     return false;
                 }

    for (int i = 0; i < 20; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto outs = findPortsMatching(
            {"InstrumentVocoder", "Vocoder", "calf"}, false);
        if (!outs.empty())
            break;
    }
    return true;
}




bool AudioGraphManager::startProcesses()
{
 // Launch everything once at instrument boot. Mode switches only rewire.
 // [instrument-bus] the bus goes first so every later node has somewhere to link to
 launchBusNode();
 bool ok = launchZyn();
 launchZynDrums();
 launchSooperLooper();
 launchVocoder(); // keep host alive; Mode 1 simply leaves it unlinked
    launchVolumeNode();  // [breath_vol_node] created once, lives for the whole run
 started_ = ok;
 // Ensure USB mic capture ports exist in PipeWire before first mode build (called before return -- was dead code)
    activateMicCapturePorts();
 ensureMvx2uMicCaptureLive();
    return ok;
}




void AudioGraphManager::stopProcesses()
{
    disconnectAllOwnedLinks();
    stopVolumeWorker();  // [breath_vol_node]
    killPid(vol_pid_, "zyn-volume-node");
    killPid(voc_pid_, "vocoder");
    killPid(sl_pid_, "sooperlooper");
    killPid(zyn_drums_pid_, "zyn-drums");
    killPid(zyn_pid_, "zyn-melody");
    killPid(bus_pid_, "instrument-bus"); // [instrument-bus]
    started_ = false;
}





// Force-remove every link this instrument cares about, including ones
// PipeWire auto-created when Zyn/SL/jalv started (those never landed in
// owned_links_, so disconnectAllOwnedLinks alone cannot clear them).
void AudioGraphManager::forceDisconnectInstrumentGraph()
{
    // [instrument-bus] Remove every link this instrument cares about, including ones PipeWire
    // auto-created when Zyn/SL/jalv started (those never landed in owned_links_): our sources ->
    // configured sink, our sources -> bus input, the bus output -> sink, and the looper/vocoder
    // feeds. Only ports of OUR clients are touched, never another application's links.
    refreshPortCache();
    const PortCache& c = port_cache_;
    auto kill = [this](const std::string& src, const std::string& dst) {
        if (!src.empty() && !dst.empty())
            pwUnlink(src, dst);
    };
    std::vector<std::string> srcs = {c.mel_l, c.mel_r, c.dr_l, c.dr_r, c.vout_l, c.vout_r,
                                     c.vol_out_l, c.vol_out_r};
    srcs.insert(srcs.end(), c.mic_capture.begin(), c.mic_capture.end());
    for (const auto& s : srcs) {
        kill(s, c.sl_in_l); kill(s, c.sl_in_r);
        kill(s, c.play_l); kill(s, c.play_r);
        kill(s, c.bus_in_l); kill(s, c.bus_in_r);
    }
    for (const auto& s : {c.mel_l, c.mel_r}) {
        kill(s, c.vin_l); kill(s, c.vin_r);
    }
    for (const auto& s : c.mic_capture) {
        kill(s, c.vin_l); kill(s, c.vin_r);
        kill(s, c.vside); kill(s, c.vside2);
    }
    // looper output -> sink / bus
    kill(c.sl_out_l, c.play_l); kill(c.sl_out_r, c.play_r);
    kill(c.sl_out_l, c.bus_in_l); kill(c.sl_out_r, c.bus_in_r);
    // bus output -> sink
    kill(c.bus_out_l, c.play_l); kill(c.bus_out_r, c.play_r);
    // Also clear owned_links_ tracking so we start clean.
    owned_links_.clear();
    Logger::info("forceDisconnectInstrumentGraph: cleared auto + owned links");
}









void AudioGraphManager::ensureHealthyGraph() {
    // ------------------------------------------------------------------
    // 1) Refresh knowledge of every entity (even if unused in this mode)
    // ------------------------------------------------------------------
    MicInfo mic = queryMic(); // updates capture port names after unplug/replug

    // Playback targets can change if the user switches sinks
    std::string play_l = defaultPlaybackPort(true);
    std::string play_r = defaultPlaybackPort(false);
    if (play_l.empty())
        play_l = PI_AUDIO_LEFT_SINK;
    if (play_r.empty())
        play_r = PI_AUDIO_RIGHT_SINK;

    // ------------------------------------------------------------------
    // 2) Ensure processes exist (relaunch only if dead -- never kill healthy)
    // ------------------------------------------------------------------
    bool need_rebuild = false;
    if (!processAlive(zyn_pid_)) {
        Logger::warning("Health: melody Zyn dead -- relaunching");
        launchZyn();
        need_rebuild = true;
    }
    if (!processAlive(zyn_drums_pid_)) {
        Logger::warning("Health: drums Zyn dead -- relaunching");
        launchZynDrums();
        need_rebuild = true;
    }
    if (!processAlive(sl_pid_)) {
            static int sl_fail_streak = 0;
            static std::chrono::steady_clock::time_point last_sl_warn{};
            auto nowh = std::chrono::steady_clock::now();
            bool should_log = (last_sl_warn.time_since_epoch().count() == 0) ||
                              (nowh - last_sl_warn) > std::chrono::seconds(8);
            if (should_log) {
                Logger::warning("Health: SooperLooper dead -- relaunching "
                                "(required in every mode; rate-limited)");
                last_sl_warn = nowh;
            }
            ++sl_fail_streak;
            bool launched = launchSooperLooper();
            if (launched) {
                need_rebuild = true;
                sl_fail_streak = 0;
            } else if (sl_fail_streak >= 3) {
                if (recoverAudioStack("repeated SooperLooper launch failure")) {
                    need_rebuild = true;
                    sl_fail_streak = 0;
                }
            }
        }
    if (!vol_unavailable_ && !processAlive(vol_pid_)) {  // [breath_vol_node]
        static auto vol_last_try = std::chrono::steady_clock::time_point{};
        auto nowv = std::chrono::steady_clock::now();
        if (vol_last_try.time_since_epoch().count() == 0 ||
            nowv - vol_last_try > std::chrono::seconds(10)) {
            vol_last_try = nowv;
            Logger::warning("Health: zyn-volume-node dead -- relaunching");
            if (launchVolumeNode()) {
                invalidatePortCache();
                need_rebuild = true;
            }
        }
    }
    // [instrument-bus] master bus lifecycle: rate-limited relaunch like zyn-volume-node, then
    // verify bus -> configured sink so a replugged / replaced USB sink recovers within a tick.
    if (!bus_unavailable_ && !processAlive(bus_pid_)) {
        static auto bus_last_try = std::chrono::steady_clock::time_point{};
        const auto nowb = std::chrono::steady_clock::now();
        if (bus_last_try.time_since_epoch().count() == 0 ||
            nowb - bus_last_try > std::chrono::seconds(10)) {
            bus_last_try = nowb;
            Logger::warning("Health: instrument-bus dead -- relaunching");
            if (launchBusNode()) {
                invalidatePortCache();
                sweep_pending_ = true;
                need_rebuild = true;
            }
        }
    }
    {
        bool bus_ready_now = false;
        if (processAlive(bus_pid_)) {
            std::string bi_l, bi_r, bo_l, bo_r;
            resolveVolumeNodePorts("instrument-bus:", bi_l, bi_r, bo_l, bo_r);
            bus_ready_now = !bi_l.empty() && !bi_r.empty() && !bo_l.empty() && !bo_r.empty();
            if (bus_ready_now && last_bus_ready_)
                ensureBusSinkLinks(bo_l, bo_r, play_l, play_r);
        }
        if (bus_ready_now != last_bus_ready_) {
            Logger::info(std::string("Health: instrument-bus is now ") +
                         (bus_ready_now ? "ready" : "gone") + " -- rebuilding routing");
            last_bus_ready_ = bus_ready_now;
            invalidatePortCache();
            sweep_pending_ = true;
            need_rebuild = true;
        }
    }
    if (!processAlive(voc_pid_)) {
        Logger::warning("Health: Vocoder host dead -- relaunching");
        launchVocoder();
        need_rebuild = true;
    }

    // ------------------------------------------------------------------
    // 3) Resolve ports; if any critical name vanished, force rebuild
    // ------------------------------------------------------------------
    std::string mel_l = resolveClientPort(cfg_.zyn_client_name, "out_1", false);
    std::string mel_r = resolveClientPort(cfg_.zyn_client_name, "out_2", false);
    if (mel_l.empty() || mel_r.empty()) {
        Logger::warning("Health: melody ports missing -- rebuild");
        need_rebuild = true;
    }
    std::string sl_in_l, sl_in_r, sl_out_l, sl_out_r;
    if (!resolveLooperPorts(sl_in_l, sl_in_r, sl_out_l, sl_out_r)) {
        Logger::warning("Health: SL ports missing -- rebuild");
        need_rebuild = true;
    }
    // Vocoder ports (always tracked; Mode 1 leaves them unlinked)
    auto voc_ports = findPorts("InstrumentVocoder", false);
    auto voc_in = findPorts("InstrumentVocoder", true);
    if (processAlive(voc_pid_) && voc_ports.empty() && voc_in.empty()) {
        // Host alive but ports not visible yet -- small wait then recheck once
        // (caller runs every 2s so next tick will catch it; still flag rebuild)
        need_rebuild = true;
    }
    // Mic: ALSA sees it but PipeWire may not have exported capture yet.
    // Activate profile in-process (no GUI). Do NOT force a full graph
    // rebuild just for this in Mode 1 -- that was causing periodic xruns.
    if (mic.present && mic.capture_ports.empty()) {
        Logger::warning("Health: mic present in ALSA but no PW capture ports yet");
        if (activateMicCapturePorts()) {
            mic = queryMic();
            // Ports just appeared: rebuild so Mode 2/3/5 can link them.
            if (mode_ == PerformanceMode::VocoderOnly
                || mode_ == PerformanceMode::SynthAndVocoder
                || mode_ == PerformanceMode::BreathOctaveMic) {
                need_rebuild = true;
            }
        }
    }

    // ------------------------------------------------------------------
    // 4) Rebuild current mode wiring only when something actually
    // changed or died (dead process, missing ports). No blind rebuild
    // on a timer: a rebuild tears down and relinks every owned
    // connection (see forceDisconnectInstrumentGraph()) and produces
    // an audible JACK xrun every time it fires, so it must only run
    // when need_rebuild says something real is actually wrong.
    // ------------------------------------------------------------------
    if (need_rebuild) { // [instrument-bus] see the bus / sink as they are now, then sweep stale direct links
        invalidatePortCache();
        sweep_pending_ = true;
    }
    PerformanceMode m = mode_;
    if (need_rebuild) {
        Logger::debug(
            std::string("Health: ensureHealthyGraph mode=")
            + std::to_string(static_cast<int>(m))
            + " rebuild=yes");
        setMode(m);
        applyMixerLevels(last_master_, last_dry_, last_vocoder_, last_drums_,
                          last_synth_, last_mic_);
    }
}




// ===== Differential mode graph (modediff) ================================
std::vector<AudioGraphManager::LinkRole>
AudioGraphManager::desiredRoles(PerformanceMode mode)
{
    using R = LinkRole;
    // [instrument-bus] BusSink (bus out -> configured sink) is in EVERY mode's list, so it
    // never changes across a mode transition. There is no Mode 6 routing any more: a stray
    // PerformanceMode::Talkbox value takes the default list below.
    switch (mode) {
    case PerformanceMode::SynthOnly:
        return {R::MelVol, R::VolSl, R::VolHp, R::DrumSl, R::DrumHp, R::SlHp, R::MidiMel,
                R::MidiDru, R::BusSink};
    case PerformanceMode::BreathOctave:
        return {R::MelSl, R::MelHp, R::DrumSl, R::DrumHp, R::SlHp, R::MidiMel, R::MidiDru,
                R::BusSink};
    case PerformanceMode::VocoderOnly:
        return {R::MelVoc, R::MicSide, R::VocSl, R::VocHp, R::DrumSl, R::DrumHp, R::SlHp,
                R::MidiMel, R::MidiDru, R::BusSink};
    case PerformanceMode::SynthAndVocoder:
        return {R::MelVoc, R::MicSide, R::VocSl, R::VocHp, R::MicSl, R::MicHp, R::DrumSl,
                R::DrumHp, R::SlHp, R::MidiMel, R::MidiDru, R::BusSink};
    case PerformanceMode::BreathOctaveMic:
        return {R::MelSl, R::MelHp, R::DrumSl, R::DrumHp, R::SlHp, R::MicSl, R::MicHp,
                R::MidiMel, R::MidiDru, R::BusSink};
    default:
        break;
    }
    return {R::MelSl, R::MelHp, R::DrumSl, R::DrumHp, R::SlHp, R::MidiMel, R::MidiDru,
            R::BusSink};
}



// ===== modespeed: port cache + pre-computed link sets =====================
void AudioGraphManager::invalidatePortCache()
{
    port_cache_.valid = false;
    for (auto& v : mode_link_cache_valid_) v = false;
}

void AudioGraphManager::refreshPortCache()
{
    PortCache& c = port_cache_;
    c = PortCache{};                         // zero

    c.mel_l = resolveClientPort(cfg_.zyn_client_name, "out_1", false);
    c.mel_r = resolveClientPort(cfg_.zyn_client_name, "out_2", false);
    c.dr_l  = resolveClientPort(cfg_.zyn_drums_client_name, "out_1", false);
    c.dr_r  = resolveClientPort(cfg_.zyn_drums_client_name, "out_2", false);
    if (c.mel_l.empty()) c.mel_l = "zynaddsubfx_zyn-melody:out_1";
    if (c.mel_r.empty()) c.mel_r = "zynaddsubfx_zyn-melody:out_2";
    if (c.dr_l.empty())  c.dr_l  = "zynaddsubfx_zyn-drums:out_1";
    if (c.dr_r.empty())  c.dr_r  = "zynaddsubfx_zyn-drums:out_2";

    if (!resolveLooperPorts(c.sl_in_l, c.sl_in_r, c.sl_out_l, c.sl_out_r)) {
        c.sl_in_l  = "sooperlooper:common_in_1";
        c.sl_in_r  = "sooperlooper:common_in_2";
        c.sl_out_l = "sooperlooper:common_out_1";
        c.sl_out_r = "sooperlooper:common_out_2";
    }

    c.play_l = defaultPlaybackPort(true);
    c.play_r = defaultPlaybackPort(false);
    if (c.play_l.empty())
        c.play_l = PI_AUDIO_LEFT_SINK;
    if (c.play_r.empty())
        c.play_r = PI_AUDIO_RIGHT_SINK;

    c.vin_l  = "InstrumentVocoder:in_l";
    c.vin_r  = "InstrumentVocoder:in_r";
    c.vside  = "InstrumentVocoder:sidechain";
    c.vside2 = "InstrumentVocoder:sidechain2";
    c.vout_l = "InstrumentVocoder:out_l";
    c.vout_r = "InstrumentVocoder:out_r";

    MicInfo mic = queryMic();
    c.mic_capture = mic.capture_ports;

    resolveVolumeNodePorts("zyn-volume-node:", c.vol_in_l, c.vol_in_r, c.vol_out_l, c.vol_out_r); // [breath_vol_node]
    // [instrument-bus] one "final destination" for every *Hp role: the bus input when the bus is
    // ready, the configured sink itself otherwise. play_l/play_r stay the configured sink.
    resolveVolumeNodePorts("instrument-bus:", c.bus_in_l, c.bus_in_r, c.bus_out_l, c.bus_out_r);
    c.bus_ready = processAlive(bus_pid_) && !c.bus_in_l.empty() && !c.bus_in_r.empty() &&
                  !c.bus_out_l.empty() && !c.bus_out_r.empty();
    c.final_l = c.bus_ready ? c.bus_in_l : c.play_l;
    c.final_r = c.bus_ready ? c.bus_in_r : c.play_r;
    c.midi_mel = findEngineMidiPort();
    c.midi_dru = findEngineDrumsMidiPort();

    c.valid = true;
    // Logger::info("modespeed: port cache refreshed");
}

const AudioGraphManager::PortCache& AudioGraphManager::portCache()
{
    if (!port_cache_.valid)
        refreshPortCache();
    return port_cache_;
}


void AudioGraphManager::ensureModeLinksCached(PerformanceMode mode)
{
    // Build the concrete list exactly once (or after invalidate).
    const int idx = static_cast<int>(mode);
    if (idx < 0 || idx >= 6) return;
    if (mode_link_cache_valid_[idx]) return;

    // Make sure processes that this mode needs are alive (cheap if already up).
    if (!processAlive(zyn_pid_))       launchZyn();
    if (!processAlive(zyn_drums_pid_)) launchZynDrums();
    if (!processAlive(sl_pid_))        launchSooperLooper();
    if (mode == PerformanceMode::VocoderOnly ||
        mode == PerformanceMode::SynthAndVocoder) {
        if (!processAlive(voc_pid_)) launchVocoder();
    }
    if (mode == PerformanceMode::BreathOctaveMic ||
        mode == PerformanceMode::SynthAndVocoder) {
        ensureMvx2uMicCaptureLive();
    }
        forceMvx2uPurePlayback();

    if (mode == PerformanceMode::SynthOnly && !processAlive(vol_pid_))
        launchVolumeNode();  // [breath_vol_node]
    // Force a fresh resolution so the cache is accurate for this mode.
    refreshPortCache();
    const PortCache& c = port_cache_;

    std::vector<std::pair<std::string,std::string>> want;
    auto add2 = [&](const std::string& a, const std::string& b,
                    const std::string& d, const std::string& e) {
        if (!a.empty() && !d.empty()) want.emplace_back(a, d);
        if (!b.empty() && !e.empty()) want.emplace_back(b, e);
    };

    const bool vol_ok = !c.vol_in_l.empty() && !c.vol_in_r.empty() &&
                        !c.vol_out_l.empty() && !c.vol_out_r.empty();  // [breath_vol_node]
    auto roles = desiredRoles(mode);
    for (auto r : roles) {
        switch (r) {
        case LinkRole::MelVol:
            if (vol_ok) add2(c.mel_l, c.mel_r, c.vol_in_l, c.vol_in_r);
            break;
        case LinkRole::VolSl:
            if (vol_ok) add2(c.vol_out_l, c.vol_out_r, c.sl_in_l, c.sl_in_r);
            break;
        case LinkRole::VolHp:
            if (vol_ok) add2(c.vol_out_l, c.vol_out_r, c.final_l, c.final_r);
            break;
        case LinkRole::MelSl:  add2(c.mel_l, c.mel_r, c.sl_in_l, c.sl_in_r); break;
        case LinkRole::MelHp:  add2(c.mel_l, c.mel_r, c.final_l, c.final_r);  break;
        case LinkRole::MelVoc: add2(c.mel_l, c.mel_r, c.vin_l,   c.vin_r);   break;
        case LinkRole::DrumSl:
            if (processAlive(zyn_drums_pid_))
                add2(c.dr_l, c.dr_r, c.sl_in_l, c.sl_in_r);
            break;
        case LinkRole::DrumHp:
            if (processAlive(zyn_drums_pid_))
                add2(c.dr_l, c.dr_r, c.final_l, c.final_r);
            break;
        case LinkRole::VocSl:  add2(c.vout_l, c.vout_r, c.sl_in_l, c.sl_in_r); break;
        case LinkRole::VocHp:  add2(c.vout_l, c.vout_r, c.final_l, c.final_r);  break;
        case LinkRole::MicSide:
            for (const auto& p : c.mic_capture) {
                if (!p.empty()) {
                    want.emplace_back(p, c.vside);
                    want.emplace_back(p, c.vside2);
                    break;
                }
            }
            break;
        case LinkRole::MicSl:
            for (const auto& p : c.mic_capture) {
                if (!p.empty()) {
                    want.emplace_back(p, c.sl_in_l);
                    want.emplace_back(p, c.sl_in_r);
                    break;
                }
            }
            break;
        case LinkRole::MicHp:
            for (const auto& p : c.mic_capture) {
                if (!p.empty()) {
                    want.emplace_back(p, c.final_l);
                    want.emplace_back(p, c.final_r);
                    break;
                }
            }
            break;
        case LinkRole::SlHp:
            add2(c.sl_out_l, c.sl_out_r, c.final_l, c.final_r);
            break;
        case LinkRole::BusSink:
            if (c.bus_ready)
                add2(c.bus_out_l, c.bus_out_r, c.play_l, c.play_r);
            break;
        case LinkRole::MidiMel: {
            std::string dst = resolveClientPort(cfg_.zyn_client_name, "midi_input", true);
            if (!c.midi_mel.empty() && !dst.empty())
                want.emplace_back(c.midi_mel, dst);
            break;
        }
        case LinkRole::MidiDru: {
            std::string dst = resolveClientPort(cfg_.zyn_drums_client_name, "midi_input", true);
            if (!c.midi_dru.empty() && !dst.empty() && processAlive(zyn_drums_pid_))
                want.emplace_back(c.midi_dru, dst);
            break;
        }
        }
    }

    if (mode == PerformanceMode::SynthOnly && !vol_ok) {
        Logger::warning("zyn-volume-node ports not found -- Mode 1 routing Zyn directly (no breath volume)");
        add2(c.mel_l, c.mel_r, c.sl_in_l, c.sl_in_r);
        add2(c.mel_l, c.mel_r, c.final_l, c.final_r);
    }
    mode_link_cache_[idx] = std::move(want);
    mode_link_cache_valid_[idx] = true;
    // Logger::info("modespeed: cached " + std::to_string(mode_link_cache_[idx].size())
    //              + " links for mode " + std::to_string(idx));
}


bool AudioGraphManager::applyModeTransition(PerformanceMode from,
                                            PerformanceMode to)
{
    // Logger::info("Mode transition " + std::to_string(static_cast<int>(from))
    //              + " -> " + std::to_string(static_cast<int>(to)));

    // Make sure both ends are cached (first time each mode is seen).
    ensureModeLinksCached(from);
    ensureModeLinksCached(to);

    if (from == to) {
        // Same-mode press: just verify the links are still there (cheap).
        const auto& want = mode_link_cache_[static_cast<int>(to)];
        for (const auto& w : want)
            pwLink(w.first, w.second);   // silent if already present
        return true;
    }

    // Pure set-difference using the two cached vectors.
    const auto& from_links = mode_link_cache_[static_cast<int>(from)];
    const auto& to_links   = mode_link_cache_[static_cast<int>(to)];

    auto in_list = [](const std::vector<std::pair<std::string,std::string>>& L,
                      const std::string& s, const std::string& d) {
        for (const auto& p : L)
            if (p.first == s && p.second == d) return true;
        return false;
    };

    // Unlink everything that is in from but not in to
    for (const auto& p : from_links) {
        if (!in_list(to_links, p.first, p.second))
            pwUnlink(p.first, p.second);
    }
    // Link everything that is in to but not in from
    for (const auto& p : to_links) {
        if (!in_list(from_links, p.first, p.second))
            pwLink(p.first, p.second);
    }

    mode_ = to;
    return true;
}

bool AudioGraphManager::setMode(PerformanceMode mode) {
    const PerformanceMode from = mode_;
    const bool ok = applyModeTransition(from, mode);
    mode_ = mode;                       // already set inside apply, but keep consistent
    // [instrument-bus] AFTER the new links exist (no silent gap): drop stale direct source -> sink links.
    if (sweep_pending_) {
        sweep_pending_ = false;
        if (port_cache_.valid && port_cache_.bus_ready)
            sweepDirectSinkLinks();
    }
    if (mode != PerformanceMode::SynthOnly)
        setBreathVolume(1.0f);  // [breath_vol_node] node fixed at 100% outside Mode 1
    switch (mode) {
    case PerformanceMode::SynthOnly:
        Logger::info("Mode SynthOnly (Mode 1) [fast]");
        break;
    case PerformanceMode::VocoderOnly:
        Logger::info("Mode VocoderOnly (Mode 2) [fast]");
        break;
    case PerformanceMode::SynthAndVocoder:
        Logger::info("Mode SynthAndVocoder (Mode 3) [fast]");
        break;
    case PerformanceMode::BreathOctave:
        Logger::info("Mode BreathOctave (Mode 4) [fast]");
        break;
    case PerformanceMode::BreathOctaveMic:
        Logger::info("Mode BreathOctaveMic (Mode 5) [fast]");
        break;
        break;
    }
    return ok;
}
std::string AudioGraphManager::statusSummary() const
{
    std::ostringstream ss;
    ss << "mode=";
    switch (mode_) {
    case PerformanceMode::SynthOnly: ss << "SynthOnly"; break;
    case PerformanceMode::VocoderOnly: ss << "VocoderOnly"; break;
    case PerformanceMode::SynthAndVocoder: ss << "SynthAndVocoder"; break;
 case PerformanceMode::BreathOctave: ss << "BreathOctave"; break;
 case PerformanceMode::BreathOctaveMic: ss << "BreathOctaveMic"; break;
     }
    ss << " melody=" << (processAlive(zyn_pid_) ? "up" : "down")
       << " drums=" << (processAlive(zyn_drums_pid_) ? "up" : "down")
       << " sl=" << (sooperlooperRunning() ? "up" : "down")
       << " voc=" << (vocoderRunning() ? "up" : "down")
       << " bus=" << (processAlive(bus_pid_) ? "up" : "down")
       << " mic=" << (micPresent() ? "yes" : "no")
       << " links=" << owned_links_.size();
    return ss.str();
}



// --- Looper OSC ---------------------------------------------------------
// SooperLooper runs with 4 independent loops (-l 4). Per-loop control
// uses /sl/<track>/hit (track 0..3); the "armed" loop (default 0,
// re-armed by Record on that track) is the target for the shared
// Overdub/Undo controls. Mute All addresses SooperLooper's broadcast
// loop index (-1) so it only silences the loops -- never live Zyn.
// Prefer `oscsend` if present; otherwise log (see sendLooperOsc()).
namespace
{
 std::string loopHitPath(int track)
 {
     return "/sl/" + std::to_string(track) + "/hit";
 }
}




void AudioGraphManager::looperRecordTrack(int track)
{
    if (track < 0 || track > 3)
        return;
    if (!processAlive(sl_pid_)) {
        if (!launchSooperLooper()) {
            Logger::info("Looper Record (SooperLooper unavailable)");
            return;
        }
    }
    connectLooperGraph();

    // Single-loop model: only track 0 holds audio. Tracks 1..3 are overdub.
    if (track == 0) {
        armed_loop_ = 0;
        cycle_master_ = 0;
        sendLooperOsc(loopHitPath(0), "s", "record");
        Logger::info("Looper: BASE record toggle on loop 0");
        return;
    }

    // Secondary "record" keys -> overdub onto the base loop.
    armed_loop_ = 0;
    cycle_master_ = 0;
    sendLooperOsc(loopHitPath(0), "s", "overdub");
    Logger::info("Looper: OVERDUB toggle on loop 0 (from track " +
                 std::to_string(track) + " key)");
}









void AudioGraphManager::looperMuteTrack(int track)
{
    if (track < 0 || track > 3)
        return;
    if (!processAlive(sl_pid_)) {
        Logger::info("Looper Mute (SooperLooper not running)");
        return;
    }
    // One audible loop only.
    (void)track;
    sendLooperOsc(loopHitPath(0), "s", "mute");
    Logger::info("Looper: mute toggle on loop 0");
}



void AudioGraphManager::looperClearTrack(int track)
{
    if (track < 0 || track > 3)
        return;
    if (!processAlive(sl_pid_)) {
        Logger::info("Looper Clear (SooperLooper not running)");
        return;
    }

    if (track == 0) {
        // Wipe the whole base loop (all overdub layers go with it).
        sendLooperOsc(loopHitPath(0), "s", "undo_all");
        // Some SL builds use "clear" instead of / in addition to undo_all.
        sendLooperOsc(loopHitPath(0), "s", "clear");
        cycle_master_ = -1;
        armed_loop_ = 0;
        for (int t = 0; t < 4; ++t) {
            loop_active_[t] = false;
            loop_recording_[t] = false;
        }
        master_cycle_sec_ = 0.0;
        master_epoch_sec_ = 0.0;
        Logger::info("Looper: CLEAR base loop 0 (all layers)");
        return;
    }

    // Secondary clear keys -> undo last overdub layer on loop 0.
    sendLooperOsc(loopHitPath(0), "s", "undo");
    Logger::info("Looper: UNDO last overdub on loop 0 (from track " +
                 std::to_string(track) + " key)");
}



void AudioGraphManager::looperMuteAll()
{
    if (!processAlive(sl_pid_)) {
        Logger::info("Looper MuteAll (SooperLooper not running)");
        return;
    }
    sendLooperOsc(loopHitPath(0), "s", "mute");
    Logger::info("Looper: mute toggle on loop 0 (MuteAll)");
}


void AudioGraphManager::looperRecord() { looperRecordTrack(0); }
void AudioGraphManager::looperClear() { looperClearTrack(0); 
    noteAllLoopsCleared();
}

void AudioGraphManager::looperOverdub()
{
    if (!processAlive(sl_pid_)) {
        Logger::info("Looper Overdub (SooperLooper not running)");
        return;
    }
    connectLooperGraph();
    armed_loop_ = 0;
    sendLooperOsc(loopHitPath(0), "s", "overdub");
    Logger::info("Looper: OVERDUB toggle on loop 0");
}


void AudioGraphManager::looperPlay()
{
    if (!processAlive(sl_pid_))
    {
        Logger::info("Looper Play (SooperLooper not running)");
        return;
    }
    // 'trigger' starts playback from the beginning; 'pause' toggles.
    sendLooperOsc(loopHitPath(armed_loop_), "s", "trigger");
}
void AudioGraphManager::looperStop()
{
    if (!processAlive(sl_pid_))
    {
        Logger::info("Looper Stop (SooperLooper not running)");
        return;
    }
    // Pause if playing; mute as a hard silence fallback.
    sendLooperOsc(loopHitPath(armed_loop_), "s", "pause");
}

void AudioGraphManager::looperUndo()
{
    if (!processAlive(sl_pid_)) {
        Logger::info("Looper Undo (SooperLooper not running)");
        return;
    }
    sendLooperOsc(loopHitPath(0), "s", "undo");
    Logger::info("Looper: UNDO on loop 0");
}




// =============================================================================
// Phase 4.2 — mic discovery + Mode 2/3 link map
// =============================================================================


std::string AudioGraphManager::resolveClientPort(
    const std::string& client_name,
    const std::string& suffix,
    bool inputs) const
{
    // PipeWire exposes `zynaddsubfx -N foo` as "zynaddsubfx_foo:port".
    std::vector<std::string> needles;
    if (!client_name.empty()) {
        needles.push_back(std::string("zynaddsubfx_") + client_name);
        needles.push_back(client_name);
    }
    needles.push_back("zynaddsubfx");

    auto ports = findPortsMatching(needles, inputs);

    std::string want = ":" + suffix; // ":out_1", ":midi_input"
    std::string want_low = want;
    for (char& c : want_low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string best_pref;
    std::string best_any;
    for (const auto& p : ports) {
        std::string low = p;
        for (char& c : low)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.find(want_low) == std::string::npos)
            continue;
        if (best_any.empty())
            best_any = p;
        // Prefer the zynaddsubfx_<client> form
        if (!client_name.empty()) {
            std::string pref = std::string("zynaddsubfx_") + client_name;
            for (char& c : pref)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (low.find(pref) != std::string::npos) {
                best_pref = p;
                break;
            }
        }
    }
    if (!best_pref.empty())
        return best_pref;
    if (!best_any.empty())
        return best_any;
    // Fallback synthetic name for logging / last-ditch link attempt
    if (!client_name.empty())
        return std::string("zynaddsubfx_") + client_name + ":" + suffix;
    return std::string("zynaddsubfx:") + suffix;
}

std::vector<std::string>
AudioGraphManager::findPortsMatching(
    const std::vector<std::string>& needles,
    bool inputs) const
{
    std::vector<std::string> result;
    std::string flag = inputs ? "-i" : "-o";
    std::string out = shellCapture("pw-link " + flag + " 2>/dev/null");
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line))
    {
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\n'))
            line.pop_back();
        if (line.empty())
            continue;
        std::string lower = line;
        for (char& c : lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& n : needles)
        {
            std::string nl = n;
            for (char& c : nl)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower.find(nl) != std::string::npos)
            {
                result.push_back(line);
                break;
            }
        }
    }
    return result;
}


AudioGraphManager::MicInfo AudioGraphManager::queryMic() const
{
    MicInfo info;
    std::string cards = shellCapture("arecord -l 2>/dev/null");
    if (cards.find(cfg_.mic_name_hint) != std::string::npos) {
        info.present = true;
        info.alsa_card_name = cfg_.mic_name_hint;
        std::istringstream css(cards);
        std::string line;
        while (std::getline(css, line)) {
            if (line.find(cfg_.mic_name_hint) == std::string::npos)
                continue;
            auto pos = line.find("card ");
            if (pos != std::string::npos) {
                int card = -1;
                if (std::sscanf(line.c_str() + pos, "card %d:", &card) == 1 && card >= 0)
                    info.alsa_hint = "hw:" + std::to_string(card) + ",0";
            }
            break;
        }
    }
    auto raw = findPortsMatching({cfg_.mic_name_hint, "Shure", "MVX2U"}, true);
    // Also scan sources (pw-link -o): real capture is alsa_input.*, not sink inputs.
    {
        auto raw_out = findPortsMatching({cfg_.mic_name_hint, "Shure", "MVX2U"}, false);
        raw.insert(raw.end(), raw_out.begin(), raw_out.end());
    }
    std::vector<std::string> preferred, secondary;
    for (const auto& p : raw) {
        std::string low = p;
        for (char& c : low)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.find("monitor") != std::string::npos) {
            info.monitor_ports.push_back(p);
            continue;
        }
        if (low.find("alsa_output") != std::string::npos)
            continue;
        if (low.find("playback_") != std::string::npos)
            continue;
        bool is_in = low.find("alsa_input") != std::string::npos ||
                     low.find("capture") != std::string::npos ||
                     low.find("input") != std::string::npos;
        bool mono = low.find("capture_mono") != std::string::npos ||
                    low.find("mono") != std::string::npos ||
                    low.find("capture_fl") != std::string::npos ||
                    low.find("capture_fr") != std::string::npos;
        if (is_in && mono)
            preferred.push_back(p);
        else if (is_in)
            secondary.push_back(p);
    }
    info.capture_ports = preferred;
    info.capture_ports.insert(info.capture_ports.end(), secondary.begin(), secondary.end());
    if (!info.capture_ports.empty())
        info.present = true;
    return info;
}

bool AudioGraphManager::activateMicCapturePorts()
{
    // Throttle: at most once per 8s (showtime-safe, no GUI).
    static auto last_try = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    if (last_try.time_since_epoch().count() != 0
        && now - last_try < std::chrono::seconds(8)) {
        return !queryMic().capture_ports.empty();
    }
    last_try = now;

    if (!queryMic().capture_ports.empty())
        return true;

    std::string cards = shellCapture("arecord -l 2>/dev/null");
    std::string cards_low = cards;
    for (char& c : cards_low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string hint_low = cfg_.mic_name_hint;
    for (char& c : hint_low)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (cards_low.find(hint_low) == std::string::npos
        && cards_low.find("shure") == std::string::npos
        && cards_low.find("mvx2u") == std::string::npos) {
        return false;
    }

    Logger::info(
        "Mic: ALSA present, no PipeWire capture ports — activating profile");

    auto sleep_ms = [](int ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };

    const char* profiles[] = {
        "pro-audio",
        "analog-stereo",
        "Analog Stereo",
        "output:analog-stereo+input:analog-stereo",
        "Analog Stereo Duplex",
        "analog-stereo-input",
        "Analog Stereo Input",
        "HiFi",
        nullptr
    };

    // --- wpctl ---
    std::string wpctl = which("wpctl");
    if (!wpctl.empty()) {
        std::string status = shellCapture(wpctl + " status 2>/dev/null");
        std::vector<std::string> ids;
        std::istringstream ss(status);
        std::string line;
        while (std::getline(ss, line)) {
            std::string low = line;
            for (char& c : low)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool mention = (low.find("mvx2u") != std::string::npos)
                           || (low.find("shure") != std::string::npos)
                           || (!hint_low.empty()
                               && low.find(hint_low) != std::string::npos);
            if (!mention)
                continue;
            for (size_t i = 0; i < line.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(line[i])))
                    continue;
                size_t j = i;
                while (j < line.size()
                       && std::isdigit(static_cast<unsigned char>(line[j])))
                    ++j;
                ids.push_back(line.substr(i, j - i));
                break;
            }
        }
        // Fallback: numeric ids in the Audio section of wpctl status
        if (ids.empty()) {
            bool in_audio = false;
            std::istringstream ss2(status);
            while (std::getline(ss2, line)) {
                if (line.find("Audio") != std::string::npos)
                    in_audio = true;
                if (in_audio && line.find("Video") != std::string::npos)
                    break;
                if (!in_audio)
                    continue;
                for (size_t i = 0; i < line.size(); ++i) {
                    if (!std::isdigit(static_cast<unsigned char>(line[i])))
                        continue;
                    size_t j = i;
                    while (j < line.size()
                           && std::isdigit(static_cast<unsigned char>(line[j])))
                        ++j;
                    ids.push_back(line.substr(i, j - i));
                    break;
                }
                if (ids.size() > 16)
                    break;
            }
        }
        for (const auto& id : ids) {
            for (int p = 0; profiles[p]; ++p) {
                std::string cmd = wpctl + " set-profile " + id + " \""
                                  + profiles[p] + "\" 2>/dev/null";
                (void)std::system(cmd.c_str());
            }
        }
        sleep_ms(700);
        if (!queryMic().capture_ports.empty()) {
            Logger::info("Mic: PipeWire capture ports active (wpctl)");
            return true;
        }
    }

    // --- pactl ---
    std::string pactl = which("pactl");
    if (!pactl.empty()) {
        std::string short_cards =
            shellCapture(pactl + " list cards short 2>/dev/null");
        std::istringstream ss(short_cards);
        std::string line;
        while (std::getline(ss, line)) {
            std::string low = line;
            for (char& c : low)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (low.find("mvx2u") == std::string::npos
                && low.find("shure") == std::string::npos
                && low.find("usb") == std::string::npos)
                continue;
            std::string card = line;
            auto sp = card.find_first_of(" \t");
            if (sp != std::string::npos)
                card = card.substr(0, sp);
            for (int p = 0; profiles[p]; ++p) {
                std::string cmd = pactl + " set-card-profile " + card + " "
                                  + profiles[p] + " 2>/dev/null";
                (void)std::system(cmd.c_str());
            }
        }
        sleep_ms(700);
        if (!queryMic().capture_ports.empty()) {
            Logger::info("Mic: PipeWire capture ports active (pactl)");
            return true;
        }
    }

    
  // Profile switching alone did not expose capture ports. That usually
  // means WirePlumber never created a node for the device at all (not
  // just the wrong profile) -- e.g. it was plugged in before the
  // session started, or a stale node is stuck. Restarting the user
  // PipeWire/WirePlumber session forces a fresh ALSA re-probe, which is
  // the actual fix in that case. Hard-throttled (60s) since it briefly
  // interrupts audio.
  static auto last_restart = std::chrono::steady_clock::time_point{};
  auto now_r = std::chrono::steady_clock::now();
  if (last_restart.time_since_epoch().count() == 0
      || now_r - last_restart > std::chrono::seconds(60)) {
    last_restart = now_r;
    std::string systemctl = which("systemctl");
    if (!systemctl.empty()) {
      Logger::warning(
          "Mic: profile switching failed -- restarting PipeWire/"
          "WirePlumber user session as last resort");
      (void)std::system((systemctl +
          " --user restart wireplumber pipewire pipewire-pulse "
          "2>/dev/null").c_str());
      sleep_ms(3000);
      if (!queryMic().capture_ports.empty()) {
        Logger::info(
            "Mic: PipeWire capture ports active after session restart");
        return true;
      }
    } else {
      Logger::warning(
          "Mic: systemctl not found -- cannot restart PipeWire session "
          "automatically");
    }
  }

  Logger::warning("Mic: still no PipeWire capture ports after profile activation");
    return false;
}




// Keep micPresent() in sync with queryMic().
// (Definition may already exist; we replace the body below.)



bool AudioGraphManager::sendLooperOscSet(int track,
                                         const std::string& prop,
                                         float value) const
{
    // SooperLooper: /sl/<n>/set  sf  <control-name> <float>
    // (NOT ssf -- that was causing every set to fail.)
    if (track < -1 || track > 3)
        return false;
    std::string oscsend_bin = which("oscsend");
    if (oscsend_bin.empty()) {
        Logger::warning("oscsend not found -- install liblo-tools for looper sync");
        return false;
    }
    std::ostringstream cmd;
    cmd << oscsend_bin << " localhost " << cfg_.sooperlooper_osc_port
        << " /sl/" << track << "/set sf " << prop << " " << value
        << " 2>/dev/null";
    int rc = std::system(cmd.str().c_str());
    if (rc != 0) {
        Logger::warning("Looper OSC set failed: /sl/" + std::to_string(track) +
                        "/set " + prop + "=" + std::to_string(value));
        return false;
    }
    Logger::info("Looper OSC set: /sl/" + std::to_string(track) +
                 "/set " + prop + "=" + std::to_string(value));
    return true;
}






void AudioGraphManager::realignActiveLoops()
{
    // no-op in overdub-only mode
}












void AudioGraphManager::applyLooperSyncTopology()
{
    // Overdub-only model: single loop, no multi-track sync topology.
    Logger::info("Looper sync: overdub-only mode (single loop 0)");
}


















void AudioGraphManager::noteLoopRecordArmed(int track)
{
    (void)track;
    armed_loop_ = 0;
    cycle_master_ = 0;
}















void AudioGraphManager::noteLoopCleared(int track)
{
    (void)track;
}












void AudioGraphManager::noteAllLoopsCleared()
{
    cycle_master_ = -1;
    for (int t = 0; t < 4; ++t) {
        loop_active_[t] = false;
        loop_recording_[t] = false;
    }
    master_cycle_sec_ = 0.0;
    master_epoch_sec_ = 0.0;
}








bool AudioGraphManager::sendLooperOsc(const std::string& path,
                                      const std::string& type_tags,
                                      const std::string& arg) const
{
    // liblo-tools: oscsend host port /path types args...
    // Example: oscsend localhost 9951 /sl/0/hit s record
    std::string oscsend_bin = which("oscsend");
    if (oscsend_bin.empty())
    {
        Logger::warning("oscsend not found — install liblo-tools for looper control");
        return false;
    }

    std::ostringstream cmd;
    cmd << oscsend_bin << " localhost " << cfg_.sooperlooper_osc_port
        << " " << path;
    if (!type_tags.empty())
        cmd << " " << type_tags;
    if (!arg.empty())
        cmd << " " << arg;
    cmd << " 2>/dev/null";

    int rc = std::system(cmd.str().c_str());
    if (rc != 0)
    {
        Logger::warning("oscsend failed: " + path + " " + type_tags + " " + arg);
        return false;
    }
    Logger::info("Looper OSC: " + path + " " + type_tags + " " + arg);
    return true;
}


void AudioGraphManager::connectLooperGraph()
{
    // [instrument-bus] Deliberately a no-op. SooperLooper's output reaches the sink ONLY through
    // the SlHp role (SL out -> instrument-bus in -> bus out -> configured sink), which the mode
    // graph owns. Linking SL straight to the sink here (this used to run on every Record /
    // Overdub press) would add a second, un-attenuated path into the sink.
}


bool AudioGraphManager::resolveLooperPorts(std::string& in_l, std::string& in_r,
                                           std::string& out_l, std::string& out_r)
{
    in_l = in_r = out_l = out_r = "";
    if (!processAlive(sl_pid_))
        return false;
    auto filt = [](std::vector<std::string> ports) {
        std::vector<std::string> o;
        for (const auto& p : ports) {
            std::string low = p;
            for (char& c : low)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (low.find("midi-bridge") != std::string::npos)
                continue;
            o.push_back(p);
        }
        return o;
    };
    auto all_in = filt(findPortsMatching(
        {"sooperlooper:common_in", "common_in", "loop0_in"}, true));
    auto all_out = filt(findPortsMatching(
        {"sooperlooper:common_out", "common_out", "loop0_out"}, false));
    auto pick = [](const std::vector<std::string>& ports, bool left) {
        for (const auto& p : ports) {
            std::string low = p;
            for (char& c : low)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (left && (low.find("_1") != std::string::npos || low.find("_l") != std::string::npos))
                return p;
            if (!left && (low.find("_2") != std::string::npos || low.find("_r") != std::string::npos))
                return p;
        }
        return ports.empty() ? std::string() : ports[0];
    };
    in_l = pick(all_in, true);
    in_r = pick(all_in, false);
    out_l = pick(all_out, true);
    out_r = pick(all_out, false);
    if (in_l.empty()) in_l = "sooperlooper:common_in_1";
    if (in_r.empty()) in_r = "sooperlooper:common_in_2";
    if (out_l.empty()) out_l = "sooperlooper:common_out_1";
    if (out_r.empty()) out_r = "sooperlooper:common_out_2";
    Logger::info("SL ports in=" + in_l + "/" + in_r + " out=" + out_l + "/" + out_r);
    return true;
}

void AudioGraphManager::linkStereoToLooper(const std::string& src_l,
                                           const std::string& src_r,
                                           const std::string& sl_in_l,
                                           const std::string& sl_in_r)
{
    if (src_l.empty() || sl_in_l.empty())
        return;
    pwLink(src_l, sl_in_l);
    if (!src_r.empty() && !sl_in_r.empty())
        pwLink(src_r, sl_in_r);
    else if (!src_r.empty())
        pwLink(src_r, sl_in_l);
}








bool AudioGraphManager::setSourceVolume(const std::string& node_name_substring,
                                         float linear)
{
    // Mirrors setNodeVolume(), but for a capture SOURCE (mic/input)
    // instead of a playback sink. A USB mic shows up in PipeWire/ALSA as
    // a source, never a sink, so setNodeVolume() alone can never find it
    // -- that was the reason the dry-mix buttons never touched real mic
    // hardware gain.
    if (linear < 0.f) linear = 0.f;
    if (linear > 1.f) linear = 1.f;

    std::string pactl = which("pactl");
    if (!pactl.empty())
    {
        std::string sources = shellCapture(pactl + " list short sources 2>/dev/null");
        std::istringstream ss(sources);
        std::string line;
        while (std::getline(ss, line))
        {
            if (line.find(node_name_substring) == std::string::npos)
                continue;
            // Skip monitor sources ("...analog-stereo.monitor") -- those
            // mirror a sink's playback, not the mic's real capture input.
            if (line.find(".monitor") != std::string::npos)
                continue;
            std::istringstream ls(line);
            std::string id, name;
            ls >> id >> name;
            int percent = static_cast<int>(linear * 100.0f + 0.5f);
            std::string setcmd = pactl + " set-source-volume " + id + " " +
                                  std::to_string(percent) + "% 2>/dev/null";
            if (std::system(setcmd.c_str()) == 0)
            {
                Logger::info("Input gain " + name + " -> " + std::to_string(percent) + "%");
                return true;
            }
        }
    }
    Logger::debug("setSourceVolume '" + node_name_substring + "' linear=" +
                  std::to_string(linear) + " (no matching source; recorded only)");
    return false;
}


void AudioGraphManager::applyMixerLevels(int master, int dry, int vocoder, int drums,
                                        int synth, int mic)
{
    last_master_ = master;
    last_dry_ = dry;
    last_vocoder_ = vocoder;
    last_drums_ = drums;
    last_synth_ = synth;
    last_mic_ = mic;
    const float d = dry / 127.0f;
    const float v = vocoder / 127.0f;
    // [instrument-bus] Master is NOT folded into the dry / vocoder levels any more. It is the gain
    // of instrument-bus only (applyMasterBus below). Nothing in this function ever changes a
    // device or sink volume.
    const float dry_final = d;
    // Vocoder gain is a fraction of the dry/voice level (relative to voice),
    // not an independent absolute -- it rides on dry_final.
    const float vocoder_final = v * dry_final;
    Logger::info(
        "Mixer master=" + std::to_string(master) + " (bus " + std::to_string((master * 100) / 127) + "%)" +
        " dry=" + std::to_string(dry) + " (" + std::to_string((dry * 100) / 127) + "%)" +
        " vocoder=" + std::to_string(vocoder) + " (" + std::to_string((vocoder * 100) / 127) + "%)" +
        " drums=" + std::to_string(drums) + " (" + std::to_string((drums * 100) / 127) + "%)");
    Logger::debug("Mixer dry_final=" + std::to_string(dry_final) +
                  " vocoder_final=" + std::to_string(vocoder_final));
    applyMasterBus(master);
    if (!cfg_.mic_name_hint.empty()) {
        // The dry-mix buttons ARE the mic's input-gain buttons: apply the
        // level directly at the USB interface's capture source, as a flat
        // fraction of whatever the mic picks up. Deliberately NOT
        // multiplied by master/vocoder -- those are downstream mix
        // decisions; this is the input stage, before anything else
        // touches the signal. dry=64 (50%) means capture gain = 50%,
        // full stop.
        const float mic_capture_gain = static_cast<float>(dry) / 127.0f;
        setSourceVolume(cfg_.mic_name_hint, mic_capture_gain);
    }
    // NOTE: 'drums' is intentionally not applied to any PipeWire node here -- drum loudness is a
    // MIDI-velocity path (Engine::handleAction, ActionType::Drum: 100 * drumMix / 127). Master is
    // NOT part of that velocity any more; the drums reach the sink through the bus like everything
    // else, so master attenuates them exactly once.
}


bool AudioGraphManager::drumsZynRunning() const
{
    return processAlive(zyn_drums_pid_);
}

// =============================================================================
// =============================================================================

bool AudioGraphManager::reloadMelodyInstrument(const std::string& xmz_path)
{
 if (xmz_path.empty()) {
 Logger::error("reloadMelodyInstrument: empty path");
 return false;
 }
 cfg_.zyn_instrument = xmz_path;
 if (processAlive(zyn_pid_)) {
 Logger::info("reloadMelodyInstrument: stopping melody Zyn to load " + xmz_path);
 killPid(zyn_pid_, "zyn-melody");
 zyn_pid_ = -1;
 std::this_thread::sleep_for(std::chrono::milliseconds(300));
 }
 const bool ok = launchZyn();
 if (ok)
 Logger::info("reloadMelodyInstrument: melody running with " + xmz_path);
 else
 Logger::error("reloadMelodyInstrument: failed to launch with " + xmz_path);
 return ok;
}

bool AudioGraphManager::forceMvx2uPurePlayback()
{
 const std::string preset_src = cfg_.mvx2u_pure_playback_preset.empty()
 ? std::string("config/NoMic.toml")
 : cfg_.mvx2u_pure_playback_preset;
 const char* home = std::getenv("HOME");
 if (home && *home) {
 const std::string dest_dir = std::string(home) + "/.config/shurectl/presets";
 const std::string dest = dest_dir + "/NoMic.toml";
 (void)std::system(("mkdir -p '" + dest_dir + "'").c_str());
 if (std::system(("cp -f '" + preset_src + "' '" + dest + "' 2>/dev/null").c_str()) == 0)
 Logger::info("MVX2U: mirrored pure-playback preset to " + dest);
 }
 Logger::info(
 "MVX2U: ensure Monitor Mix = pure Playback (NoMic preset). "
 "If the jack has mic bleed: shurectl --device /dev/hidraw1 and load NoMic.");
 return true;
}


bool AudioGraphManager::ensureMvx2uMicCaptureLive()
{
 // Mic CAPTURE into the Pi only. Does not change Monitor Mix (jack blend)
 // or phantom power (HID / hardware preset).
 //
 // Symptom this fixes: ALSA "Microphone" Capture left at 0% so every mic
 // mode is silent even though PipeWire links look correct.

 // ALSA capture gain (card name and numeric fallback)
 (void)std::system("amixer -c MVX2U sset Microphone 80% unmute 2>/dev/null");
 (void)std::system("amixer -c 3 sset Microphone 80% unmute 2>/dev/null");

 // PipeWire source: unmute + set volume on every "Shure MVX2U" source node
 (void)std::system(
 "wpctl status 2>/dev/null | "
 "sed -n 's/.*\\([0-9][0-9]*\\)\\. Shure MVX2U.*/\\1/p' | "
 "while read -r id; do "
 "wpctl set-mute \"$id\" 0 2>/dev/null; "
 "wpctl set-volume \"$id\" 0.85 2>/dev/null; "
 "done");

 Logger::info("MVX2U: mic capture forced live (ALSA Microphone 80% + PW unmute)");
 return true;
}


// ===== [zynvol] MIDI-controlled volume node ================================
// zynvol is a tiny JACK/PipeWire client ("zyn-volume-node", stereo in/out) whose gain is
// set by MIDI from MidiEngine's "MIDI Volume" port. Same node/port names as the old
// pw-loopback node, so all link roles and the direct-routing fallback are unchanged.
bool AudioGraphManager::launchVolumeNode()
{
    if (processAlive(vol_pid_))
        return true;
    if (vol_unavailable_)
        return false;
    std::string bin;
    {
        char self[4096];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = '\0';
            const std::string exe(self);
            const size_t slash = exe.rfind('/');
            if (slash != std::string::npos) {
                const std::string cand = exe.substr(0, slash + 1) + "zynvol";
                if (access(cand.c_str(), X_OK) == 0)
                    bin = cand;
            }
        }
    }
    if (bin.empty() && access("build/zynvol", X_OK) == 0)
        bin = "build/zynvol";
    if (bin.empty())
        bin = which("zynvol");
    if (bin.empty()) {
        vol_unavailable_ = true;
        Logger::warning("zynvol not found (was it built? needs libjack-jackd2-dev) -- "
                        "Mode 1 will route Zyn directly (no breath volume)");
        return false;
    }
    const std::string pwjack = which("pw-jack");
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        setenv("PIPEWIRE_PROPS", "{ node.autoconnect=false }", 1);
        if (!pwjack.empty())
            execlp("pw-jack", "pw-jack", bin.c_str(), "--name", "zyn-volume-node",
                   "--exponent", "3.0", static_cast<char*>(nullptr));
        execlp(bin.c_str(), bin.c_str(), "--name", "zyn-volume-node",
               "--exponent", "3.0", static_cast<char*>(nullptr));
        _exit(127);
    }
    vol_pid_ = pid;
    Logger::info("Launched zynvol (MIDI volume node) pid=" + std::to_string(pid));
    bool ready = false;
    for (int i = 0; i < 25 && !ready; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!processAlive(vol_pid_))
            break;
        ready = !findPortsMatching({"zyn-volume-node:"}, true).empty() &&
                !findPortsMatching({"zyn-volume-node:"}, false).empty();
    }
    if (ready)
        Logger::info("zynvol ports visible (zyn-volume-node)");
    else
        Logger::warning("zynvol ports not visible yet");
    return true;
}

// Kept only so the header stays unchanged: there is no worker thread any more.
void AudioGraphManager::volumeWorkerLoop() {}

void AudioGraphManager::stopVolumeWorker()
{
    if (vol_thread_.joinable())
        vol_thread_.join();
}

// Breath volume now travels as MIDI (Engine -> MidiEngine -> zynvol); nothing to do here.
void AudioGraphManager::setBreathVolume(float linear)
{
    if (linear < 0.f) linear = 0.f;
    if (linear > 1.f) linear = 1.f;
    vol_target_pct_.store(static_cast<int>(linear * 100.0f + 0.5f));
}

void AudioGraphManager::resolveVolumeNodePorts(const std::string& client_prefix,
                                               std::string& in_l, std::string& in_r,
                                               std::string& out_l, std::string& out_r) const
{
    // client_prefix is e.g. "zyn-volume-node:" or "instrument-bus:".
    in_l.clear(); in_r.clear(); out_l.clear(); out_r.clear();
    auto lower = [](std::string s) {
        for (char& ch : s)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    auto ends = [](const std::string& s, const char* suf) {
        const size_t n = std::strlen(suf);
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    auto pick = [&](const std::vector<std::string>& ports, std::string& l, std::string& r) {
        for (const auto& p : ports) {
            const std::string low = lower(p);
            if (low.find("monitor") != std::string::npos)
                continue;
            if (l.empty() && (ends(low, "_fl") || ends(low, "_l") || ends(low, "_1")))
                l = p;
            else if (r.empty() && (ends(low, "_fr") || ends(low, "_r") || ends(low, "_2")))
                r = p;
        }
    };
    pick(findPortsMatching({client_prefix}, true), in_l, in_r);
    pick(findPortsMatching({client_prefix}, false), out_l, out_r);
}

// ===== [instrument-bus] master bus ==========================================
// "instrument-bus" is a second instance of the zynvol tool (tools/zynvol), driven by
// MidiEngine's "MIDI Bus Volume" port. Every audible source of the instrument links ONLY to
// instrument-bus:in_1/in_2. The single path from the instrument into the physical sink is
// instrument-bus:out_1/out_2 -> the configured sink (PI_AUDIO_LEFT_SINK / PI_AUDIO_RIGHT_SINK).
// Master volume is the gain of this node; no sink or device volume is ever touched.
void AudioGraphManager::setBusVolumeSender(std::function<void(float)> fn)
{
    bus_volume_sender_ = std::move(fn);
}

bool AudioGraphManager::busRunning() const
{
    return processAlive(bus_pid_);
}

void AudioGraphManager::applyMasterBus(int master)
{
    if (master < 0) master = 0;
    if (master > 127) master = 127;
    last_master_ = master;
    if (bus_volume_sender_)
        bus_volume_sender_(static_cast<float>(master) / 127.0f);
}

bool AudioGraphManager::launchBusNode()
{
    if (processAlive(bus_pid_))
        return true;
    if (bus_unavailable_)
        return false;
    std::string bin;
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        const std::string exe(self);
        const size_t slash = exe.rfind('/');
        if (slash != std::string::npos) {
            const std::string cand = exe.substr(0, slash + 1) + "zynvol";
            if (access(cand.c_str(), X_OK) == 0)
                bin = cand;
        }
    }
    if (bin.empty() && access("build/zynvol", X_OK) == 0)
        bin = "build/zynvol";
    if (bin.empty())
        bin = which("zynvol");
    if (bin.empty()) {
        bus_unavailable_ = true;
        Logger::warning("zynvol not found (was it built? needs libjack-jackd2-dev) -- "
                        "instrument-bus unavailable: sources link straight to the configured "
                        "sink (no master volume)");
        return false;
    }
    const std::string pwjack = which("pw-jack");
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        setenv("PIPEWIRE_PROPS", "{ node.autoconnect=false }", 1);
        if (!pwjack.empty())
            execlp("pw-jack", "pw-jack", bin.c_str(), "--name", "instrument-bus",
                   "--midi-port", "MIDI Bus Volume", "--exponent", "3.0",
                   static_cast<char*>(nullptr));
        execlp(bin.c_str(), bin.c_str(), "--name", "instrument-bus",
               "--midi-port", "MIDI Bus Volume", "--exponent", "3.0",
               static_cast<char*>(nullptr));
        _exit(127);
    }
    bus_pid_ = pid;
    Logger::info("Launched instrument-bus (zynvol) pid=" + std::to_string(pid));
    bool ready = false;
    for (int i = 0; i < 25 && !ready; ++i) { // up to ~5 s for the in/out ports
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!processAlive(bus_pid_))
            break;
        ready = !findPortsMatching({"instrument-bus:"}, true).empty() &&
                !findPortsMatching({"instrument-bus:"}, false).empty();
    }
    if (!processAlive(bus_pid_)) {
        Logger::error("instrument-bus exited right after launch -- sources will link straight "
                      "to the configured sink until it is back");
        bus_pid_ = -1;
        last_bus_ready_ = false;
        return false;
    }
    last_bus_ready_ = ready;
    if (ready) {
        Logger::info("instrument-bus ports visible");
        // zynvol subscribes to the "MIDI Bus Volume" port within ~0.5 s of starting, and
        // Engine re-sends the level on every 2 s health tick, so a missed first message heals.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        applyMasterBus(last_master_);
    } else {
        Logger::warning("instrument-bus ports not visible yet");
    }
    return true;
}

// Every link PipeWire currently has, as (output port, input port). pw-link -l prints each
// port with its peers ("  |-> peer" under an output port, "  |<- peer" under an input port);
// both forms are read and de-duplicated so either direction alone is enough.
std::vector<std::pair<std::string, std::string>> AudioGraphManager::listLinks() const
{
    std::vector<std::pair<std::string, std::string>> links;
    const std::string out = shellCapture("pw-link -l 2>/dev/null");
    std::istringstream ss(out);
    std::string line, cur;
    auto add = [&links](const std::string& a, const std::string& b) {
        const std::pair<std::string, std::string> p(a, b);
        for (const auto& q : links)
            if (q == p)
                return;
        links.push_back(p);
    };
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty())
            continue;
        const size_t first = line.find_first_not_of(" \t");
        if (first == 0) { // port header
            cur = line;
            continue;
        }
        const std::string body = line.substr(first);
        const bool out_link = body.compare(0, 3, "|->") == 0;
        const bool in_link = body.compare(0, 3, "|<-") == 0;
        if ((!out_link && !in_link) || cur.empty())
            continue;
        const size_t b = body.find_first_not_of(" \t", 3);
        if (b == std::string::npos)
            continue;
        const std::string peer = body.substr(b);
        if (out_link)
            add(cur, peer);
        else
            add(peer, cur);
    }
    return links;
}

// Unlink OUR sources from the configured sink ports (and nothing else). Once the bus exists the
// only path into the sink is bus out -> sink, so any direct melody / drums / vocoder / mic / SL /
// volume-node link to the sink is stale: PipeWire auto-links, or links created before the bus
// came up (the mode-transition diff cannot see those after a cache invalidation).
void AudioGraphManager::sweepDirectSinkLinks()
{
    const PortCache& c = portCache();
    if (!c.bus_ready)
        return; // bus down: direct source -> sink links ARE the (fallback) routing
    auto low = [](std::string s) {
        for (char& ch : s)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    std::vector<std::string> exact = {c.mel_l, c.mel_r, c.dr_l, c.dr_r, c.vout_l, c.vout_r,
                                      c.sl_out_l, c.sl_out_r, c.vol_out_l, c.vol_out_r};
    exact.insert(exact.end(), c.mic_capture.begin(), c.mic_capture.end());
    const std::string mel_pref = low("zynaddsubfx_" + cfg_.zyn_client_name + ":");
    const std::string dru_pref = low("zynaddsubfx_" + cfg_.zyn_drums_client_name + ":");
    auto ours = [&](const std::string& src) {
        const std::string s = low(src);
        if (s.compare(0, 15, "instrument-bus:") == 0)
            return false;
        for (const auto& e : exact)
            if (!e.empty() && e == src)
                return true;
        return s.compare(0, mel_pref.size(), mel_pref) == 0 ||
               s.compare(0, dru_pref.size(), dru_pref) == 0 ||
               s.compare(0, 12, "sooperlooper") == 0 ||
               s.compare(0, 18, "instrumentvocoder:") == 0 ||
               s.compare(0, 16, "zyn-volume-node:") == 0;
    };
    int removed = 0;
    for (const auto& l : listLinks()) {
        if (l.second != c.play_l && l.second != c.play_r)
            continue;
        if (!ours(l.first))
            continue;
        if (pwUnlink(l.first, l.second))
            ++removed;
    }
    if (removed > 0)
        Logger::info("instrument-bus: removed " + std::to_string(removed) +
                     " direct source -> sink link(s); the bus is the only path");
}

// Verify (and repair) the one path into the sink: bus out -> configured sink. A different
// *playback* target of the bus (stale sink after a replug, or a fallback sink used while the
// configured one was away) is dropped so there is never a duplicate path; non-playback targets
// (e.g. a recorder the user attached to the bus output) are left alone.
bool AudioGraphManager::ensureBusSinkLinks(const std::string& bus_out_l, const std::string& bus_out_r,
                                           const std::string& sink_l, const std::string& sink_r)
{
    if (bus_out_l.empty() || bus_out_r.empty() || sink_l.empty() || sink_r.empty())
        return false;
    bool have_l = false, have_r = false;
    for (const auto& l : listLinks()) {
        const bool is_l = (l.first == bus_out_l);
        const bool is_r = (l.first == bus_out_r);
        if (!is_l && !is_r)
            continue;
        const std::string& want = is_l ? sink_l : sink_r;
        if (l.second == want) {
            if (is_l)
                have_l = true;
            else
                have_r = true;
            continue;
        }
        std::string low = l.second;
        for (char& ch : low)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (low.find("playback_") != std::string::npos)
            pwUnlink(l.first, l.second);
    }
    // Only try when the sink port really exists (it is absent while a USB sink is unplugged).
    if (!have_l && !findPortsMatching({sink_l}, true).empty())
        pwLink(bus_out_l, sink_l);
    if (!have_r && !findPortsMatching({sink_r}, true).empty())
        pwLink(bus_out_r, sink_r);
    return true;
}
