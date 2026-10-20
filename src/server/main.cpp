// music-service: the backend process. Everything except signal wiring lives in
// run_command so it can be exercised in tests.

#include "server/commands.hpp"

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::atomic<bool> stop_requested{false};

extern "C" void on_signal(int) { stop_requested.store(true); }

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, on_signal);
#endif
    std::vector<std::string> args(argv + 1, argv + argc);
    music_service::server::CommandHooks hooks;
    hooks.should_stop = [] { return stop_requested.load(); };
    return music_service::server::run_command(args, {std::cout, std::cerr}, std::move(hooks));
}
