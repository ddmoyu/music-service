#pragma once

#include <string>
#include <vector>

namespace music_service::server {

// Runs a serve command line under the Windows Service Control Manager (Windows only).
// The service reports START_PENDING, RUNNING once ready, and STOP_PENDING then STOPPED;
// a stop or shutdown control requests the same shutdown as Ctrl+C. Logs go to
// <data_dir>/logs/music-service.log; errors before that file exists (such as an invalid
// configuration) go to the Application event log under the source "music-service".
// The exit code is reported as the service-specific exit code. When the process was
// not started by the SCM, prints how to run it in a console and returns exit_usage.
int run_windows_service(std::vector<std::string> args);

} // namespace music_service::server
