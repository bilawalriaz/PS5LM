// Host check for control.cpp: the control port on a PC, in front of a fake
// llama-server (scripts/control-host-test.sh starts both and drives them with
// scripts/ps5lm_ctl.py). This stands in for main.cpp's frame loop: it takes the
// control port's actions, and a restart is an exit with 75 (control.cpp's
// LoadExec off the console), which the test script takes as "start me again".
//
//   control_host <dir> <control port> <server port>
//   <dir>/stuck present at start: the loop never takes an action, so the
//   control thread's fallback has to restart it
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "control.hpp"
#include "stats.hpp"

int main(int argc, char ** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: control_host <dir> <control port> <server port>\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string dir = argv[1];
    const int port = std::atoi(argv[2]), server_port = std::atoi(argv[3]);
    const bool stuck = access((dir + "/stuck").c_str(), F_OK) == 0;

    ps5lm::StatsCollector stats;
    stats.start(server_port);
    ps5lm::ModelRow small, big;
    small.file = dir + "/models/Small-Q8_0.gguf";
    small.label = "Small-Q8_0";
    small.fits = true;
    small.plan = "4k context";
    big.file = dir + "/models/Huge-F16.gguf";
    big.label = "Huge-F16";
    big.plan = "needs 40 GiB";
    stats.set_models({ small, big });
    ps5lm::Live model;
    model.model_label = small.label;
    model.model_file = small.file;
    model.ctx = 4096;
    stats.set_model(model);
    stats.set_state(ps5lm::ServerState::loading);

    ps5lm::Control control;
    control.start(port, server_port, dir.c_str(), &stats);
    std::printf("control_host: up%s\n", stuck ? ", main loop stuck" : "");
    for (;;) {
        usleep(16 * 1000);
        ps5lm::ControlAction a;
        while (!stuck && control.take(&a)) {
            if (a.kind == ps5lm::ControlAction::settings) {
                std::printf("control_host: settings: ctx_cap %u introspect %d\n", a.saved.ctx_cap, a.saved.introspect);
            } else if (a.kind == ps5lm::ControlAction::restart) {
                std::printf("control_host: restart (%s): %s / %s\n", a.hard ? "hard" : "clean", a.busy.c_str(),
                            a.detail.c_str());
                if (!a.hard) {
                    sleep(1);  // models.stop()
                }
                std::fflush(nullptr);
                _exit(75);
            }
        }
    }
}
