// music-service: the backend process. Everything except signal wiring lives in
// run_command so it can be exercised in tests.

#include "infrastructure/filesystem/paths.hpp"
#include "server/commands.hpp"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::atomic<bool> stop_requested{false};

extern "C" void on_signal(int) { stop_requested.store(true); }

} // namespace

// Commands take UTF-8. On Windows the narrow argv is in the ANSI code page, so the
// wide one is converted instead.
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.push_back(music_service::infrastructure::filesystem::path_to_utf8(std::filesystem::path{argv[i]}));
    }
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, on_signal);
#endif
    music_service::server::CommandHooks hooks;
    hooks.should_stop = [] { return stop_requested.load(); };
    return music_service::server::run_command(args, {std::cout, std::cerr}, std::move(hooks));
}
