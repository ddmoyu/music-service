#include "server/windows_service.hpp"

#include "server/commands.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <iostream>
#include <sstream>
#include <utility>

namespace music_service::server {

namespace {

// The SCM calls into these from its own threads; one service per process.
std::vector<std::string> service_args;
std::atomic<bool> stop_requested{false};
std::atomic<int> exit_code{exit_failed};
SERVICE_STATUS_HANDLE status_handle = nullptr;
SERVICE_STATUS status{};
CRITICAL_SECTION status_lock;

// Covers the soft shutdown budget and the listeners, database and lock that follow.
constexpr DWORD stop_wait_hint_ms = 60'000;

void report(DWORD state, DWORD win32_exit = NO_ERROR, DWORD service_exit = 0, DWORD wait_hint = 0) {
    EnterCriticalSection(&status_lock);
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : static_cast<DWORD>(0);
    status.dwWin32ExitCode = win32_exit;
    status.dwServiceSpecificExitCode = service_exit;
    status.dwWaitHint = wait_hint;
    status.dwCheckPoint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : status.dwCheckPoint + 1;
    SetServiceStatus(status_handle, &status);
    LeaveCriticalSection(&status_lock);
}

// Text the service could not write to its own log, for the Application event log.
void report_event(const std::string& text) {
    const auto source = RegisterEventSourceW(nullptr, L"music-service");
    if (source == nullptr) {
        return;
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    const wchar_t* strings[] = {wide.c_str()};
    ReportEventW(source, EVENTLOG_ERROR_TYPE, 0, 1, nullptr, 1, 0, strings, nullptr);
    DeregisterEventSource(source);
}

DWORD WINAPI control_handler(DWORD control, DWORD, void*, void*) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        stop_requested.store(true);
        report(SERVICE_STOP_PENDING, NO_ERROR, 0, stop_wait_hint_ms);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI service_main(DWORD, LPWSTR*) {
    status_handle = RegisterServiceCtrlHandlerExW(L"", control_handler, nullptr);
    if (status_handle == nullptr) {
        return;
    }
    report(SERVICE_START_PENDING, NO_ERROR, 0, 30'000);

    CommandHooks hooks;
    hooks.should_stop = [] { return stop_requested.load(); };
    hooks.on_ready = [](const StartupReport&) { report(SERVICE_RUNNING); };
    hooks.log_to_file = true;
    std::ostringstream out;
    std::ostringstream err;
    const int code = run_command(service_args, {out, err}, std::move(hooks));
    exit_code.store(code);
    if (code != exit_ok && !err.str().empty()) {
        report_event("music-service did not start: " + err.str());
    }
    report(SERVICE_STOPPED, code == exit_ok ? static_cast<DWORD>(NO_ERROR) : static_cast<DWORD>(ERROR_SERVICE_SPECIFIC_ERROR),
           static_cast<DWORD>(code));
}

} // namespace

int run_windows_service(std::vector<std::string> args) {
    service_args = std::move(args);
    InitializeCriticalSection(&status_lock);
    // An own-process service ignores the name in its dispatch table.
    wchar_t name[] = L"";
    SERVICE_TABLE_ENTRYW table[] = {{name, service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        const auto error = GetLastError();
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            std::cerr << "error: the service command is for the Windows service manager; "
                         "use serve to run in a console\n";
            return exit_usage;
        }
        std::cerr << "error: cannot connect to the service manager (" << error << ")\n";
        return exit_failed;
    }
    return exit_code.load();
}

} // namespace music_service::server
