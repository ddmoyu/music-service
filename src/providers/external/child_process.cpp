#include "providers/external/child_process.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace music_service::providers::external {

namespace {

constexpr std::chrono::milliseconds poll_interval{10};
// How long the destructor waits for a killed tree before giving up on it.
constexpr std::chrono::milliseconds destructor_wait{5000};

domain::Error spawn_error(std::string message) {
    return domain::Error{domain::ErrorCode::feature_unavailable, std::move(message), false, {}};
}

} // namespace

#ifdef _WIN32

namespace {

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    }
    return out;
}

// Quotes one argument the way CommandLineToArgvW and the MSVC runtime split it.
void append_argument(std::wstring& line, const std::wstring& arg) {
    if (!line.empty()) {
        line += L' ';
    }
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += arg;
        return;
    }
    line += L'"';
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            line.append(backslashes * 2 + 1, L'\\');
        } else {
            line.append(backslashes, L'\\');
        }
        backslashes = 0;
        line += c;
    }
    line.append(backslashes * 2, L'\\');
    line += L'"';
}

// Sorted, double-NUL-terminated UTF-16 block, as CreateProcessW expects.
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& environment) {
    std::vector<std::wstring> entries;
    for (const auto& [name, value] : environment) {
        entries.push_back(widen(name) + L"=" + widen(value));
    }
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto& entry : entries) {
        block += entry;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

struct Handle {
    HANDLE value{nullptr};
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() noexcept {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
        value = nullptr;
    }
    HANDLE release() noexcept { return std::exchange(value, nullptr); }
};

} // namespace

struct ChildProcess::Native {
    Handle job;
    Handle process;
    Handle stdin_write;
    Handle stdout_read;
    Handle stderr_read;
    std::atomic<bool> stdin_open{true};
};

domain::Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const ProcessSpec& spec) {
    if (!spec.program.is_absolute()) {
        return spawn_error("helper program must be an absolute path");
    }
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle in_read, in_write, out_read, out_write, err_read, err_write;
    if (!CreatePipe(&in_read.value, &in_write.value, &inheritable, 0) ||
        !CreatePipe(&out_read.value, &out_write.value, &inheritable, 0) ||
        !CreatePipe(&err_read.value, &err_write.value, &inheritable, 0)) {
        return spawn_error("could not create helper pipes");
    }
    // The parent's ends stay private.
    SetHandleInformation(in_write.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read.value, HANDLE_FLAG_INHERIT, 0);

    Handle job{CreateJobObjectW(nullptr, nullptr)};
    if (job.value == nullptr) {
        return spawn_error("could not create a job object for the helper");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return spawn_error("could not configure the helper job object");
    }

    // Only the three pipe ends are inherited, whatever else the service has open.
    HANDLE inherited[3] = {in_read.value, out_write.value, err_write.value};
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<unsigned char> attribute_storage(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        return spawn_error("could not prepare helper start attributes");
    }
    struct AttributeGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttributeGuard() { DeleteProcThreadAttributeList(list); }
    } attribute_guard{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                   nullptr, nullptr)) {
        return spawn_error("could not restrict helper handle inheritance");
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = in_read.value;
    startup.StartupInfo.hStdOutput = out_write.value;
    startup.StartupInfo.hStdError = err_write.value;
    startup.lpAttributeList = attributes;

    const auto program = spec.program.wstring();
    std::wstring command_line;
    append_argument(command_line, program);
    for (const auto& arg : spec.args) {
        append_argument(command_line, widen(arg));
    }
    auto environment = environment_block(spec.environment);
    const auto directory = spec.working_directory.wstring();

    PROCESS_INFORMATION info{};
    if (!CreateProcessW(program.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
                            EXTENDED_STARTUPINFO_PRESENT,
                        environment.data(), directory.empty() ? nullptr : directory.c_str(),
                        &startup.StartupInfo, &info)) {
        return spawn_error("helper program could not be started (error " + std::to_string(GetLastError()) + ")");
    }
    Handle process{info.hProcess};
    Handle thread{info.hThread};
    // Into the job before it runs a single instruction, so nothing it starts escapes.
    if (!AssignProcessToJobObject(job.value, process.value)) {
        TerminateProcess(process.value, 1);
        WaitForSingleObject(process.value, INFINITE);
        return spawn_error("helper could not be placed in its job object");
    }
    ResumeThread(thread.value);

    auto native = std::make_unique<Native>();
    native->job.value = job.release();
    native->process.value = process.release();
    native->stdin_write.value = in_write.release();
    native->stdout_read.value = out_read.release();
    native->stderr_read.value = err_read.release();
    // The child's ends close here; the pipes end when the tree's copies are gone.
    return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(native)));
}

bool ChildProcess::write_stdin(std::string_view data) {
    if (!native_->stdin_open) {
        return false;
    }
    while (!data.empty()) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(data.size(), 64 * 1024));
        if (!WriteFile(native_->stdin_write.value, data.data(), chunk, &written, nullptr)) {
            return false;
        }
        data.remove_prefix(written);
    }
    return true;
}

void ChildProcess::close_stdin() noexcept {
    if (native_->stdin_open.exchange(false)) {
        native_->stdin_write.reset();
    }
}

namespace {

std::size_t read_pipe(HANDLE pipe, char* buffer, std::size_t size) {
    DWORD got = 0;
    if (!ReadFile(pipe, buffer, static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD)), &got, nullptr)) {
        return 0; // ERROR_BROKEN_PIPE: every writer has gone
    }
    return got;
}

} // namespace

std::size_t ChildProcess::read_stdout(char* buffer, std::size_t size) {
    return read_pipe(native_->stdout_read.value, buffer, size);
}

std::size_t ChildProcess::read_stderr(char* buffer, std::size_t size) {
    return read_pipe(native_->stderr_read.value, buffer, size);
}

void ChildProcess::request_stop() noexcept {}

void ChildProcess::kill_tree() noexcept {
    TerminateJobObject(native_->job.value, 1);
}

bool ChildProcess::wait_tree_exit(std::chrono::milliseconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (QueryInformationJobObject(native_->job.value, JobObjectBasicAccountingInformation, &accounting,
                                      sizeof(accounting), nullptr) &&
            accounting.ActiveProcesses == 0 && WaitForSingleObject(native_->process.value, 0) == WAIT_OBJECT_0) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(poll_interval);
    }
}

std::optional<int> ChildProcess::exit_code() const noexcept {
    DWORD code = 0;
    if (WaitForSingleObject(native_->process.value, 0) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(native_->process.value, &code)) {
        return std::nullopt;
    }
    return static_cast<int>(code);
}

#else

namespace {

struct Fd {
    int value{-1};
    Fd() = default;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { reset(); }
    void reset() noexcept {
        if (value >= 0) {
            ::close(value);
        }
        value = -1;
    }
    int release() noexcept { return std::exchange(value, -1); }
};

bool make_pipe(Fd& read_end, Fd& write_end) {
    int fds[2];
#ifdef __linux__
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        return false;
    }
#else
    if (::pipe(fds) != 0) {
        return false;
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
    read_end.value = fds[0];
    write_end.value = fds[1];
    return true;
}

// A helper that stops reading must not take the service down with SIGPIPE; the
// failed write is reported instead. On Linux the service also becomes the subreaper
// of what helpers leave behind, so their orphans are reaped and counted as gone even
// when the service runs as PID 1 in a container.
void prepare_process_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction current {};
        if (::sigaction(SIGPIPE, nullptr, &current) == 0 && current.sa_handler == SIG_DFL) {
            struct sigaction ignore {};
            ignore.sa_handler = SIG_IGN;
            ::sigemptyset(&ignore.sa_mask);
            ::sigaction(SIGPIPE, &ignore, nullptr);
        }
#ifdef __linux__
        ::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
#endif
    });
}

} // namespace

struct ChildProcess::Native {
    pid_t pid{-1};
    std::atomic<bool> reaped{false};
    std::atomic<int> status{0};
    std::mutex wait_mutex;
    Fd stdin_write;
    Fd stdout_read;
    Fd stderr_read;
    std::atomic<bool> stdin_open{true};
};

domain::Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const ProcessSpec& spec) {
    if (!spec.program.is_absolute()) {
        return spawn_error("helper program must be an absolute path");
    }
    prepare_process_once();
    Fd in_read, in_write, out_read, out_write, err_read, err_write, exec_read, exec_write;
    if (!make_pipe(in_read, in_write) || !make_pipe(out_read, out_write) || !make_pipe(err_read, err_write) ||
        !make_pipe(exec_read, exec_write)) {
        return spawn_error("could not create helper pipes");
    }

    // Everything the child needs is built before fork: after it only async-signal-safe
    // calls are allowed.
    const auto program = spec.program.string();
    std::vector<std::string> arg_storage{program};
    arg_storage.insert(arg_storage.end(), spec.args.begin(), spec.args.end());
    std::vector<char*> argv;
    for (auto& arg : arg_storage) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    std::vector<std::string> env_storage;
    for (const auto& [name, value] : spec.environment) {
        env_storage.push_back(name + "=" + value);
    }
    std::vector<char*> envp;
    for (auto& entry : env_storage) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    const auto directory = spec.working_directory.string();
    const long open_max = ::sysconf(_SC_OPEN_MAX);
    const int max_fd = static_cast<int>(std::clamp<long>(open_max, 256, 65536));

    const pid_t pid = ::fork();
    if (pid < 0) {
        return spawn_error("could not fork the helper");
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(in_read.value, STDIN_FILENO);
        ::dup2(out_write.value, STDOUT_FILENO);
        ::dup2(err_write.value, STDERR_FILENO);
        for (int fd = 3; fd < max_fd; ++fd) {
            if (fd != exec_write.value) {
                ::close(fd);
            }
        }
        sigset_t none;
        ::sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::signal(SIGPIPE, SIG_DFL);
        if (!directory.empty() && ::chdir(directory.c_str()) != 0) {
            const int error = errno;
            [[maybe_unused]] auto ignored = ::write(exec_write.value, &error, sizeof(error));
            ::_exit(127);
        }
        ::execve(argv[0], argv.data(), envp.data());
        const int error = errno;
        [[maybe_unused]] auto ignored = ::write(exec_write.value, &error, sizeof(error));
        ::_exit(127);
    }
    // Also from the parent, so a kill aimed at the group can never miss the child.
    ::setpgid(pid, pid);
    in_read.reset();
    out_write.reset();
    err_write.reset();
    exec_write.reset();

    int exec_error = 0;
    ssize_t got = 0;
    do {
        got = ::read(exec_read.value, &exec_error, sizeof(exec_error));
    } while (got < 0 && errno == EINTR);
    if (got > 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
        return spawn_error(std::string{"helper program could not be started: "} + std::strerror(exec_error));
    }

    auto native = std::make_unique<Native>();
    native->pid = pid;
    native->stdin_write.value = in_write.release();
    native->stdout_read.value = out_read.release();
    native->stderr_read.value = err_read.release();
    return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(native)));
}

bool ChildProcess::write_stdin(std::string_view data) {
    if (!native_->stdin_open) {
        return false;
    }
    while (!data.empty()) {
        const auto written = ::write(native_->stdin_write.value, data.data(), data.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

void ChildProcess::close_stdin() noexcept {
    if (native_->stdin_open.exchange(false)) {
        native_->stdin_write.reset();
    }
}

namespace {

std::size_t read_fd(int fd, char* buffer, std::size_t size) {
    for (;;) {
        const auto got = ::read(fd, buffer, size);
        if (got >= 0) {
            return static_cast<std::size_t>(got);
        }
        if (errno != EINTR) {
            return 0;
        }
    }
}

} // namespace

std::size_t ChildProcess::read_stdout(char* buffer, std::size_t size) {
    return read_fd(native_->stdout_read.value, buffer, size);
}

std::size_t ChildProcess::read_stderr(char* buffer, std::size_t size) {
    return read_fd(native_->stderr_read.value, buffer, size);
}

void ChildProcess::request_stop() noexcept {
    ::kill(-native_->pid, SIGTERM);
}

void ChildProcess::kill_tree() noexcept {
    ::kill(-native_->pid, SIGKILL);
}

bool ChildProcess::wait_tree_exit(std::chrono::milliseconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        {
            const std::lock_guard lock{native_->wait_mutex};
            // Reap the leader and, as subreaper on Linux, every orphan of the group.
            int status = 0;
            pid_t reaped = 0;
            while ((reaped = ::waitpid(-native_->pid, &status, WNOHANG)) > 0) {
                if (reaped == native_->pid) {
                    native_->status = status;
                    native_->reaped = true;
                }
            }
            if (!native_->reaped && ::waitpid(native_->pid, &status, WNOHANG) == native_->pid) {
                native_->status = status;
                native_->reaped = true;
            }
            // Only after the leader is reaped can an empty group be told apart.
            if (native_->reaped && ::kill(-native_->pid, 0) != 0 && errno == ESRCH) {
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(poll_interval);
    }
}

std::optional<int> ChildProcess::exit_code() const noexcept {
    if (!native_->reaped) {
        return std::nullopt;
    }
    const int status = native_->status;
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return WIFSIGNALED(status) ? -WTERMSIG(status) : -1;
}

#endif

ChildProcess::ChildProcess(std::unique_ptr<Native> native) : native_(std::move(native)) {}

ChildProcess::~ChildProcess() {
    close_stdin();
    if (!wait_tree_exit(std::chrono::milliseconds{0})) {
        kill_tree();
        (void)wait_tree_exit(destructor_wait);
    }
}

} // namespace music_service::providers::external
