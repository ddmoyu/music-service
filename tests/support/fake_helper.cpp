// A stand-in helper program for the external helper tests (T26). argv[1] picks the
// behaviour, argv[2] (optional) is a file that receives this process's id and, in
// spawn_child mode, the grandchild's. Speaks protocol 1 by hand, no libraries.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

constexpr const char* tool_version = "1.0-test";

long long pid() {
#ifdef _WIN32
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

void record_pid(const char* file, long long id) {
    if (file != nullptr) {
        std::ofstream out{file, std::ios::app};
        out << id << "\n";
    }
}

bool has_environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    const bool found = _dupenv_s(&value, &size, name) == 0 && value != nullptr;
    std::free(value);
    return found;
#else
    return std::getenv(name) != nullptr;
#endif
}

void say(const std::string& line) {
    std::cout << line << "\n" << std::flush;
}

void hello(const char* version = tool_version, int protocol = 1) {
    say(std::string{"{\"type\":\"hello\",\"protocol\":"} + std::to_string(protocol) +
        ",\"implementation\":\"fake-helper\",\"implementation_version\":\"0.1\",\"tool\":\"fake\","
        "\"tool_version\":\"" + version + "\",\"operations\":[\"extract_audio\"]}");
}

std::string request_id(const std::string& request) {
    const std::string key = "\"request_id\":\"";
    const auto at = request.find(key);
    if (at == std::string::npos) {
        return {};
    }
    const auto start = at + key.size();
    return request.substr(start, request.find('"', start) - start);
}

std::string read_request() {
    std::string line;
    std::getline(std::cin, line);
    return line;
}

void sleep_forever() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds{1});
    }
}

std::string result(const std::string& id, const std::string& url, const std::string& extractor,
                   const std::string& media_id) {
    const auto expires = static_cast<long long>(std::time(nullptr)) + 600;
    return "{\"type\":\"result\",\"request_id\":\"" + id + "\",\"result\":{\"url\":\"" + url +
           "\",\"extractor\":\"" + extractor + "\",\"media_id\":\"" + media_id +
           "\",\"container\":\"m4a\",\"codec\":\"aac\",\"bitrate_kbps\":128,\"duration_ms\":3000,"
           "\"expires_at\":" + std::to_string(expires) +
           ",\"headers\":{\"User-Agent\":\"fake\",\"Cookie\":\"c=1\"}}}";
}

// Leaves once stdin closes, as a finished helper should.
void wait_for_eof() {
    std::string ignored;
    while (std::getline(std::cin, ignored)) {
    }
}

void spawn_grandchild(const char* self, const char* pid_file) {
#ifdef _WIN32
    (void)self;
    wchar_t module[MAX_PATH];
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring command = L"\"" + std::wstring{module} + L"\" sleeper";
    if (pid_file != nullptr) {
        std::string file{pid_file};
        command += L" \"" + std::wstring(file.begin(), file.end()) + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    // Inherits stdout like a careless tool would, so the pipe stays open until the
    // whole tree is gone.
    if (CreateProcessW(module, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &info)) {
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
#else
    (void)self;
    if (::fork() == 0) {
        std::signal(SIGTERM, SIG_IGN);
        record_pid(pid_file, pid());
        sleep_forever();
    }
#endif
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "ok";
    const char* pid_file = argc > 2 ? argv[2] : nullptr;
    if (mode == "sleeper") {
        record_pid(pid_file, pid());
        sleep_forever();
    }
    record_pid(pid_file, pid());

    if (mode == "exit_early") {
        std::cerr << "fake helper: cannot start, see https://example.invalid/help?token=abc\n";
        return 3;
    }
    if (mode == "slow_hello") {
        std::this_thread::sleep_for(std::chrono::seconds{30});
        return 0;
    }
    if (mode == "garbage") {
        say("this is not json");
        wait_for_eof();
        return 0;
    }
    if (mode == "wrong_version") {
        hello("9.9");
        wait_for_eof();
        return 0;
    }
    if (mode == "wrong_protocol") {
        hello(tool_version, 2);
        wait_for_eof();
        return 0;
    }
    if (mode == "flood") {
        hello();
        (void)read_request();
        const std::string chunk(64 * 1024, 'x');
        for (int i = 0; i < 64; ++i) {
            std::cout << chunk;
        }
        std::cout << std::flush;
        sleep_forever();
    }

    hello();
    const auto request = read_request();
    const auto id = request_id(request);

    if (mode == "ok") {
        say(result(id, "https://media.example.com/audio/a.m4a?sig=1", "testx", "abc"));
        wait_for_eof();
        return 0;
    }
    if (mode == "env") {
        // Reports whether the service's own environment leaked into the helper.
        const bool leaked = has_environment("MUSIC_SERVICE_TEST_SECRET");
        const bool protocol = has_environment("MUSIC_HELPER_PROTOCOL");
        say(result(id, "https://media.example.com/a.m4a", "testx",
                   std::string{leaked ? "leaked" : "clean"} + (protocol ? "-protocol" : "")));
        wait_for_eof();
        return 0;
    }
    if (mode == "stderr_flood") {
        const std::string chunk(4096, 'e');
        for (int i = 0; i < 256; ++i) {
            std::cerr << chunk;
        }
        std::cerr << std::flush;
        say(result(id, "https://media.example.com/a.m4a", "testx", "abc"));
        wait_for_eof();
        return 0;
    }
    if (mode == "bad_url") {
        say(result(id, "https://evil.example.org/a.m4a", "testx", "abc"));
        wait_for_eof();
        return 0;
    }
    if (mode == "generic") {
        say(result(id, "https://media.example.com/a.m4a", "generic", "abc"));
        wait_for_eof();
        return 0;
    }
    if (mode == "rate_limited") {
        say("{\"type\":\"error\",\"request_id\":\"" + id +
            "\",\"error\":{\"code\":\"rate_limited\",\"message\":\"slow down\",\"retry_after_seconds\":30}}");
        wait_for_eof();
        return 0;
    }
    if (mode == "crash") {
        std::cerr << "fake helper: crashing\n" << std::flush;
        std::_Exit(70);
    }
    if (mode == "cooperative") {
        // Waits for the cancel message or the end of stdin, then leaves on its own.
        wait_for_eof();
        return 0;
    }
    if (mode == "stubborn" || mode == "spawn_child") {
#ifndef _WIN32
        std::signal(SIGTERM, SIG_IGN);
#endif
        if (mode == "spawn_child") {
            spawn_grandchild(argv[0], pid_file);
        }
        sleep_forever();
    }
    return 2;
}
