#pragma once

#include "domain/result.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace music_service::providers::external {

struct ProcessSpec {
    std::filesystem::path program; // absolute; never looked up on PATH
    std::vector<std::string> args; // argv[1..]; no shell is involved
    std::filesystem::path working_directory;
    // The complete environment of the child; nothing is inherited.
    std::vector<std::pair<std::string, std::string>> environment;
};

// One child process and everything it starts, with stdin, stdout and stderr on pipes
// and no other handle inherited. The whole tree is one unit: a Windows job object
// (killed when the last handle closes, so helpers never outlive the service) or a POSIX
// process group. Thread-safe as follows: one thread may write stdin, one read stdout
// and one read stderr at a time; the control calls may come from any thread.
class ChildProcess {
public:
    static domain::Result<std::unique_ptr<ChildProcess>> spawn(const ProcessSpec& spec);

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    // Kills the tree and waits for it to go.
    ~ChildProcess();

    // Writes everything or returns false once the child no longer reads.
    bool write_stdin(std::string_view data);
    void close_stdin() noexcept;

    // Blocking reads; 0 at end of stream (the pipe's last writer has gone). Reads end
    // when the tree is killed.
    std::size_t read_stdout(char* buffer, std::size_t size);
    std::size_t read_stderr(char* buffer, std::size_t size);

    // Asks the tree to stop: SIGTERM to the group on POSIX. Windows has no such signal
    // for console-less children; there closing stdin and the protocol's cancel are the
    // request.
    void request_stop() noexcept;
    // Forcibly ends every process of the tree.
    void kill_tree() noexcept;

    // True once every process of the tree has ended. Never reports an exit earlier.
    [[nodiscard]] bool wait_tree_exit(std::chrono::milliseconds timeout);
    // The direct child's exit code once it has exited (negative: killed by a signal).
    [[nodiscard]] std::optional<int> exit_code() const noexcept;

private:
    struct Native;
    explicit ChildProcess(std::unique_ptr<Native> native);

    std::unique_ptr<Native> native_;
};

} // namespace music_service::providers::external
