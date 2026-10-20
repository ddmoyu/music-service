#pragma once

#include "server/process_host.hpp"

#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace music_service::server {

struct Console {
    std::ostream& out; // results and the startup report; never secrets
    std::ostream& err; // diagnostics
};

struct CommandHooks {
    // serve: polled until it returns true; then the service stops.
    std::function<bool()> should_stop;
    // serve: called once the service is ready.
    std::function<void(const StartupReport&)> on_ready;
    HostOptions host;
};

// Exit codes of the music-service command line.
inline constexpr int exit_ok = 0;
inline constexpr int exit_failed = 1; // the operation failed (including INSTANCE_LOCKED)
inline constexpr int exit_usage = 2;  // bad arguments or configuration

// Runs one command line (without the program name):
//   [--config FILE] [--data-dir DIR] [COMMAND]
//   serve (default) | version | check-config | init | healthcheck
//   principal create --name NAME | principal list | principal enable|disable --id ID
//   token issue --principal ID --scopes A,B --output FILE | token list --principal ID
//   token revoke --id ID
// Management commands run offline: they take the instance lock and fail while the
// service runs. Token plaintext only ever goes to the new file named by --output (or
// the bootstrap file for init), never to the console.
int run_command(const std::vector<std::string>& args, Console console, CommandHooks hooks = {});

} // namespace music_service::server
