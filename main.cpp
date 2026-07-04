// main.cpp — belltracker JACK client entry point
// Hardware: Raspberry Pi 5 + Steinberg UR22mkII (audio + MIDI DIN out)
//
// Start JACK first:
//   jackd -d alsa -d hw:UR22mkII -r 48000 -p 256 -n 3 &
// Then:
//   ./build/belltracker

#include "belltracker.h"
#include <jack/jack.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>

static BellTracker*   g_tracker = nullptr;
static jack_client_t* g_client  = nullptr;

static void signal_handler(int) {
    printf("\nShutting down...\n");
    if (g_client) { jack_deactivate(g_client); jack_client_close(g_client); }
    delete g_tracker;
    exit(0);
}

static int jack_process_cb(jack_nframes_t nframes, void* arg) {
    auto* bt = static_cast<BellTracker*>(arg);
    float* in = static_cast<float*>(
        jack_port_get_buffer(bt->input_port, nframes));
    bt->process(in, nframes);
    return 0;
}

static void jack_shutdown_cb(void*) {
    fprintf(stderr, "JACK shut down (UR22mkII unplugged?) — finalizing...\n");
    // Deactivate first so the RT callback stops before we touch shared state.
    if (g_client) { jack_deactivate(g_client); jack_client_close(g_client); g_client = nullptr; }
    delete g_tracker; g_tracker = nullptr;  // destructor calls stop_recording()
    exit(0);
}

int main(int argc, char** argv) {
    std::string load_cal_path;
    std::string save_cal_path = CAL_FILE_DEFAULT;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--load-cal", 0) == 0) {
            auto eq = arg.find('=');
            load_cal_path = (eq != std::string::npos) ? arg.substr(eq + 1)
                                                        : CAL_FILE_DEFAULT;
        } else if (arg.rfind("--save-cal=", 0) == 0) {
            save_cal_path = arg.substr(std::string("--save-cal=").size());
        } else if (arg == "--debug" || arg == "-d") {
            g_debug = true;
        } else if (arg == "--help" || arg == "-h") {
            printf("Usage: %s [--load-cal[=PATH]] [--save-cal=PATH]\n", argv[0]);
            printf("  --load-cal[=PATH]  Skip CAL phase, load saved bell data from PATH\n");
            printf("                     (default: %s) and enter PERF mode directly.\n",
                   CAL_FILE_DEFAULT);
            printf("  --save-cal=PATH    Path CAL data is saved to on completion\n");
            printf("                     (default: %s)\n", CAL_FILE_DEFAULT);
            printf("  --debug, -d        Verbose diagnostics: state transitions, waiting\n");
            printf("                     heartbeat w/ RMS, strike analysis, NMF activations.\n");
            printf("                     Bench use only — may cost occasional xruns.\n");
            return 0;
        }
    }

    // line-buffer stdout: under systemd/journalctl stdout is a pipe (block-
    // buffered by default) — without this, debug output lags by kilobytes
    setvbuf(stdout, nullptr, _IOLBF, 0);

    printf("belltracker v1.2  —  Handbell MIDI tracker\n");
    if (g_debug) printf("  DEBUG instrumentation ON\n");
    printf("  Audio: Steinberg UR22mkII via JACK\n");
    printf("  MIDI:  UR22mkII DIN out via ALSA sequencer\n");
    printf("==========================================\n\n");

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    jack_status_t status;
    g_client = jack_client_open("belltracker", JackNoStartServer, &status);
    if (!g_client) {
        fprintf(stderr, "Cannot connect to JACK (is jackd running?)\n");
        fprintf(stderr, "  jackd -d alsa -d hw:UR22mkII -r 48000 -p 256 -n 3 &\n");
        return 1;
    }

    if ((int)jack_get_sample_rate(g_client) != SAMPLE_RATE) {
        fprintf(stderr, "JACK sample rate %u ≠ expected %d\n",
                jack_get_sample_rate(g_client), SAMPLE_RATE);
        jack_client_close(g_client);
        return 1;
    }

    g_tracker = new BellTracker();
    g_tracker->jack_client_ = g_client;

    g_tracker->input_port = jack_port_register(
        g_client, "in",
        JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);

    if (!g_tracker->input_port) {
        fprintf(stderr, "Cannot register JACK input port\n");
        delete g_tracker; jack_client_close(g_client);
        return 1;
    }

    jack_set_process_callback(g_client, jack_process_cb, g_tracker);
    jack_on_shutdown(g_client, jack_shutdown_cb, nullptr);

    if (!g_tracker->init(load_cal_path, save_cal_path)) {
        delete g_tracker; jack_client_close(g_client);
        return 1;
    }

    if (jack_activate(g_client) != 0) {
        fprintf(stderr, "Cannot activate JACK client\n");
        delete g_tracker; jack_client_close(g_client);
        return 1;
    }

    // Auto-connect to UR22mkII first capture port
    const char** ports = jack_get_ports(
        g_client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsPhysical | JackPortIsOutput);
    if (ports) {
        if (jack_connect(g_client, ports[0],
                         jack_port_name(g_tracker->input_port)) == 0)
            printf("Audio connected: %s\n\n", ports[0]);
        else
            fprintf(stderr, "Warning: could not auto-connect audio from %s\n", ports[0]);
        jack_free(ports);
    }

    g_tracker->notify_ready();
    printf("Ready.\n\n");
    while (true) sleep(1);
    return 0;
}
