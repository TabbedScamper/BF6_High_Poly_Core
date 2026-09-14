#pragma once

#include "rime_state_bridge.h"

#include <string>
#include <vector>

namespace rime_state {

struct LiveCaptureOptions {
    std::string python_executable;
    std::string capture_tool;
    std::string game_directory;
    std::string route;
    int max_depth = 6;
    std::vector<std::string> explicit_host_inputs;
};

struct LiveCaptureReport {
    bool process_started = false;
    unsigned long exit_code = ~0ul;
    WireReport wire;
    ApplyReport applied;
};

/* Launch the current-install evaluator directly (never through a shell), read
 * its validated state from an anonymous pipe, and apply it in memory. */
bool capture_and_apply(rime::Screen& screen, const LiveCaptureOptions& options,
                       LiveCaptureReport& report, std::string& error);

/* Resolve the development defaults plus optional explicit environment
 * overrides BF6_UI_STATE_PYTHON, BF6_UI_STATE_TOOL and BF6_UI_STATE_HOSTS.
 * HOSTS is pipe-delimited; every entry is forwarded as one --host argument. */
bool discover_live_capture_options(const std::string& game_directory,
                                   const std::string& route,
                                   LiveCaptureOptions& options,
                                   std::string& error);

} // namespace rime_state
