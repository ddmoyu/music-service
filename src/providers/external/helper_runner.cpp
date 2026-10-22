#include "providers/external/helper_runner.hpp"

#include "infrastructure/filesystem/paths.hpp"
#include "providers/external/child_process.hpp"
#include "providers/external/helper_protocol.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace music_service::providers::external {

namespace ports = application::ports;
namespace cfg = application::config;
using SteadyTime = std::chrono::steady_clock::time_point;
using std::chrono::milliseconds;

namespace {

// Room for the hello line on top of the reply budget.
constexpr std::uint64_t hello_allowance = 64 * 1024;
// After a kill, how long one wait lasts before the kill is repeated.
constexpr milliseconds kill_wait{5000};

domain::Error unsupported(std::string message) {
    return domain::Error{domain::ErrorCode::capability_unsupported, std::move(message), false, {}};
}

domain::Error unavailable(std::string message) {
    // The installed helper does not match the configuration: an operator problem that
    // retrying does not fix.
    return domain::Error{domain::ErrorCode::feature_unavailable, std::move(message), false, {{"helper", "unavailable"}}};
}

domain::Error failed(std::string message, bool retryable, std::string reason) {
    return domain::Error{domain::ErrorCode::upstream_failed, std::move(message), retryable,
                         {{"helper", std::move(reason)}}};
}

std::int64_t elapsed_ms(SteadyTime since) {
    return std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - since).count();
}

std::uint64_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

// The child's complete environment: enough for a program to run, nothing of the
// service's own (tokens, proxies, configuration).
std::vector<std::pair<std::string, std::string>> helper_environment(const std::filesystem::path& work) {
    const auto dir = infrastructure::filesystem::path_to_utf8(work);
    std::vector<std::pair<std::string, std::string>> env;
#ifdef _WIN32
    for (const char* name : {"SystemRoot", "windir", "SystemDrive"}) {
        char* value = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&value, &size, name) == 0 && value != nullptr) {
            env.emplace_back(name, value);
        }
        std::free(value);
    }
    env.emplace_back("TEMP", dir);
    env.emplace_back("TMP", dir);
    env.emplace_back("USERPROFILE", dir);
#else
    env.emplace_back("PATH", "/usr/local/bin:/usr/bin:/bin");
    env.emplace_back("HOME", dir);
    env.emplace_back("TMPDIR", dir);
    env.emplace_back("LANG", "C.UTF-8");
#endif
    env.emplace_back("MUSIC_HELPER_PROTOCOL", std::to_string(ports::helper_protocol_version));
    return env;
}

// Output of one helper as its reader threads collect it.
struct Streams {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<std::string> lines;
    std::string partial;
    std::uint64_t stdout_bytes{0};
    bool stdout_closed{false};
    bool overflow{false};
    std::string stderr_tail;
};

enum class Wait { line, closed, overflow, timeout, canceled };

} // namespace

// One helper process from spawn to confirmed exit.
struct HelperRunner::Session {
    std::unique_ptr<ChildProcess> process;
    std::shared_ptr<Streams> streams = std::make_shared<Streams>();
    std::jthread stdout_reader;
    std::jthread stderr_reader;
    bool finished{false};

    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // The readers end only when the tree is gone, so it must be gone before they join.
    ~Session() {
        if (process && !finished) {
            finish(milliseconds{0}, {});
        }
    }

    void start_readers(std::uint64_t stdout_limit, std::uint64_t stderr_limit) {
        auto* child = process.get();
        stdout_reader = std::jthread([child, s = streams, stdout_limit] {
            std::vector<char> buffer(8192);
            for (;;) {
                const auto got = child->read_stdout(buffer.data(), buffer.size());
                const std::lock_guard lock{s->mutex};
                if (got == 0) {
                    s->stdout_closed = true;
                    s->changed.notify_all();
                    return;
                }
                s->stdout_bytes += got;
                if (s->stdout_bytes > stdout_limit) {
                    // Stop reading: the helper blocks on its pipe until it is killed.
                    s->overflow = true;
                    s->changed.notify_all();
                    return;
                }
                for (std::size_t i = 0; i < got; ++i) {
                    if (buffer[i] == '\n') {
                        if (!s->partial.empty() && s->partial.back() == '\r') {
                            s->partial.pop_back();
                        }
                        s->lines.push_back(std::exchange(s->partial, {}));
                    } else {
                        s->partial += buffer[i];
                    }
                }
                s->changed.notify_all();
            }
        });
        // stderr is drained to the end so the helper never blocks on it; only the
        // last stderr_limit bytes are kept for diagnostics.
        stderr_reader = std::jthread([child, s = streams, stderr_limit] {
            std::vector<char> buffer(4096);
            for (;;) {
                const auto got = child->read_stderr(buffer.data(), buffer.size());
                if (got == 0) {
                    return;
                }
                const std::lock_guard lock{s->mutex};
                s->stderr_tail.append(buffer.data(), got);
                if (s->stderr_tail.size() > stderr_limit) {
                    s->stderr_tail.erase(0, s->stderr_tail.size() - static_cast<std::size_t>(stderr_limit));
                }
            }
        });
    }

    Wait next_line(SteadyTime until, const std::stop_token& stop, std::string& line) {
        std::unique_lock lock{streams->mutex};
        const bool woke = streams->changed.wait_until(lock, stop, until, [&] {
            return !streams->lines.empty() || streams->stdout_closed || streams->overflow;
        });
        if (!streams->lines.empty()) {
            line = std::move(streams->lines.front());
            streams->lines.pop_front();
            return Wait::line;
        }
        if (streams->overflow) {
            return Wait::overflow;
        }
        if (streams->stdout_closed) {
            return Wait::closed;
        }
        if (!woke && stop.stop_requested()) {
            return Wait::canceled;
        }
        return Wait::timeout;
    }

    // Returns once every process of the tree is gone. graceful: the helper finished or
    // is asked to stop and gets grace to exit on its own before it is killed.
    void finish(milliseconds grace, std::string_view cancel_line) {
        if (!cancel_line.empty()) {
            (void)process->write_stdin(cancel_line);
        }
        process->close_stdin();
        if (!cancel_line.empty()) {
            process->request_stop();
        }
        if (!process->wait_tree_exit(grace)) {
            // Nothing is returned until the tree has really gone; a kill is repeated
            // rather than abandoned.
            do {
                process->kill_tree();
            } while (!process->wait_tree_exit(kill_wait));
        }
        stdout_reader = {};
        stderr_reader = {};
        finished = true;
    }

    std::string stderr_tail() {
        const std::lock_guard lock{streams->mutex};
        return streams->stderr_tail;
    }
};

HelperRunner::HelperRunner(HelperRunnerOptions options, std::shared_ptr<ports::UpstreamAdmissionPort> admission,
                           std::shared_ptr<const ports::Clock> clock,
                           std::optional<infrastructure::observability::Logger> log)
    : options_(std::move(options)), admission_(std::move(admission)), clock_(std::move(clock)),
      log_(std::move(log)) {}

domain::Result<ports::HelperOutcome<ports::HelperAudioLocation>>
HelperRunner::extract_audio(const ports::HelperCall& call, const ports::HelperExtractRequest& request,
                            const ports::OperationContext& context) {
    const auto found = options_.adapters.find(call.adapter);
    if (found == options_.adapters.end()) {
        return unsupported("no external adapter is configured as " + call.adapter);
    }
    const auto& adapter = found->second;
    if (adapter.source != call.base.source.value()) {
        return unsupported("external adapter " + call.adapter + " does not serve source " + call.base.source.value());
    }
    const auto operation = std::string{ports::helper_operation_extract_audio};
    if (std::find(adapter.operations.begin(), adapter.operations.end(), operation) == adapter.operations.end()) {
        return unsupported("external adapter " + call.adapter + " does not allow " + operation);
    }
    const HelperScope scope{adapter.extractors, adapter.domains};
    if (auto link = check_link(request.link, scope); !link) {
        return std::move(link).error();
    }
    if (context.stop.stop_requested()) {
        return domain::Error{domain::ErrorCode::canceled, "canceled before the helper started", false, {}};
    }

    // One process and the tool's declared connection bound, reserved together and
    // released only after the process tree has exited.
    auto admission_request = call.base;
    if (admission_request.operation.empty()) {
        admission_request.operation = operation;
    }
    admission_request.origin = "helper://" + call.adapter;
    admission_request.connections = adapter.max_connections;
    admission_request.helper_processes = 1;
    admission_request.deadline = context.deadline;
    auto admitted = admission_->try_acquire(admission_request);
    if (!admitted) {
        return std::move(admitted).error();
    }
    if (auto* deferred = std::get_if<ports::Deferred>(&admitted.value())) {
        return ports::HelperOutcome<ports::HelperAudioLocation>{*deferred};
    }
    auto lease = std::get<ports::UpstreamLease>(std::move(admitted).value());

    const auto request_id = "hlp_" + std::to_string(process_id()) + "_" + std::to_string(++next_id_);
    const auto work = options_.work_root / request_id;
    std::error_code ec;
    std::filesystem::create_directories(work, ec);
    if (ec) {
        return domain::Error{domain::ErrorCode::file_system, "could not create the helper working directory", true,
                             {}};
    }
    const auto started_at = std::chrono::steady_clock::now();
    auto result = [&]() -> domain::Result<HelperReply> {
        Session session;
        auto spawned = ChildProcess::spawn({adapter.program, adapter.args, work, helper_environment(work)});
        if (!spawned) {
            return unavailable("external adapter " + call.adapter + " could not start: " + spawned.error().message);
        }
        ++started_;
        session.process = std::move(spawned).value();
        session.start_readers(options_.max_response_bytes + hello_allowance, adapter.max_stderr_bytes);

        // Every path below ends in finish(), which returns only after the tree exited.
        const auto stop_now = [&](domain::Error error) -> domain::Result<HelperReply> {
            session.finish(adapter.stop_grace, encode_cancel(request_id));
            return error;
        };
        const auto broken = [&](domain::Error error) -> domain::Result<HelperReply> {
            session.finish(milliseconds{0}, {});
            return error;
        };
        const auto wait_failure = [&](Wait wait, bool deadline_hit, std::string_view phase) -> domain::Error {
            switch (wait) {
            case Wait::canceled:
                return domain::Error{domain::ErrorCode::canceled, "helper canceled", false, {}};
            case Wait::timeout:
                if (deadline_hit) {
                    return domain::Error{domain::ErrorCode::deadline_exceeded,
                                         "deadline passed while waiting for the helper", false, {}};
                }
                return domain::Error{domain::ErrorCode::upstream_timeout,
                                     "helper did not answer in time (" + std::string{phase} + ")", true,
                                     {{"helper", "timeout"}}};
            case Wait::overflow:
                return failed("helper wrote more than the response budget", false, "output_limit");
            case Wait::closed:
            case Wait::line:
                break;
            }
            return failed("helper exited before it replied", true, "exited");
        };

        // Hello: protocol and tool version must be exactly what was configured.
        const auto hello_until = std::min(context.deadline, std::chrono::steady_clock::now() + adapter.startup_timeout);
        std::string line;
        if (const auto wait = session.next_line(hello_until, context.stop, line); wait != Wait::line) {
            auto error = wait_failure(wait, hello_until == context.deadline, "hello");
            return wait == Wait::closed || wait == Wait::overflow ? broken(std::move(error)) : stop_now(std::move(error));
        }
        auto hello = parse_hello(line);
        if (!hello) {
            return broken(std::move(hello).error());
        }
        const auto& identity = hello.value();
        if (identity.protocol != ports::helper_protocol_version) {
            return broken(unavailable("external adapter " + call.adapter + " speaks protocol " +
                                      std::to_string(identity.protocol) + ", the service needs " +
                                      std::to_string(ports::helper_protocol_version)));
        }
        if (identity.tool_version != adapter.tool_version) {
            return broken(unavailable("external adapter " + call.adapter + " reports tool version " +
                                      sanitize_diagnostic(identity.tool_version, 64) + ", configured " +
                                      adapter.tool_version));
        }
        if (std::find(identity.operations.begin(), identity.operations.end(), operation) == identity.operations.end()) {
            return broken(unsupported("external adapter " + call.adapter + " does not offer " + operation));
        }

        const auto now = std::chrono::steady_clock::now();
        const auto reply_until = std::min(context.deadline, now + adapter.operation_timeout);
        const HelperBudget budget{std::chrono::duration_cast<milliseconds>(reply_until - now), adapter.max_connections,
                                  options_.max_response_bytes};
        if (!session.process->write_stdin(encode_request(request_id, operation, budget, scope, request))) {
            return broken(failed("helper stopped reading its input", true, "exited"));
        }
        if (const auto wait = session.next_line(reply_until, context.stop, line); wait != Wait::line) {
            auto error = wait_failure(wait, reply_until == context.deadline, "reply");
            return wait == Wait::closed || wait == Wait::overflow ? broken(std::move(error)) : stop_now(std::move(error));
        }
        auto reply = parse_reply(line, request_id);
        // A finished helper exits on its own once stdin closes; stop_grace bounds that.
        session.finish(adapter.stop_grace, {});
        if (log_) {
            const auto tail = session.stderr_tail();
            if (!tail.empty() && (!reply || reply.value().error)) {
                log_->warn("helper.stderr", {{"adapter", call.adapter},
                                             {"request_id", request_id},
                                             {"stderr", sanitize_diagnostic(tail, adapter.max_stderr_bytes)}});
            }
        }
        return reply;
    }();
    // The tree has exited: only now does the quota come back.
    lease.release();
    std::filesystem::remove_all(work, ec);

    if (log_) {
        log_->info("helper.finished", {{"adapter", call.adapter},
                                       {"request_id", request_id},
                                       {"duration_ms", elapsed_ms(started_at)},
                                       {"outcome", result && !result.value().error
                                                       ? std::string{"ok"}
                                                       : std::string{domain::to_string(
                                                             result ? result.value().error->code
                                                                    : result.error().code)}}});
    }
    if (!result) {
        return std::move(result).error();
    }
    auto reply = std::move(result).value();
    if (reply.error) {
        if (reply.retry_after) {
            admission_->feedback(admission_request, clock_->steady_now() + *reply.retry_after);
        }
        return std::move(*reply.error);
    }
    auto location = parse_audio_location(*reply.result_json, scope, clock_->now());
    if (!location) {
        return std::move(location).error();
    }
    return ports::HelperOutcome<ports::HelperAudioLocation>{std::move(location).value()};
}

} // namespace music_service::providers::external
