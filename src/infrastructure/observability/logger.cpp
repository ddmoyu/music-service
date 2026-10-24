#include "infrastructure/observability/logger.hpp"

#include "infrastructure/observability/redaction.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <ctime>
#include <type_traits>
#include <utility>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#endif

namespace music_service::infrastructure::observability {

namespace {

LogField redact(LogField field) {
    if (auto* text = std::get_if<std::string>(&field.value)) {
        *text = redact_field(field.key, *text);
    }
    return field;
}

nlohmann::json to_json(const LogValue& value) {
    return std::visit([](const auto& v) { return nlohmann::json(v); }, value);
}

// Keeps a text log record on one line: control characters and backslashes are escaped.
std::string escape_text(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '\\': result.append("\\\\"); break;
        case '\n': result.append("\\n"); break;
        case '\r': result.append("\\r"); break;
        case '\t': result.append("\\t"); break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
                char buffer[5];
                std::snprintf(buffer, sizeof buffer, "\\x%02x", static_cast<unsigned char>(c));
                result.append(buffer);
            } else {
                result.push_back(c);
            }
        }
    }
    return result;
}

std::string to_text(const LogValue& value) {
    return std::visit(
        [](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return v;
            } else if constexpr (std::is_same_v<T, bool>) {
                return v ? "true" : "false";
            } else {
                return std::to_string(v);
            }
        },
        value);
}

} // namespace

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::trace:
        return "trace";
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warn:
        return "warn";
    case LogLevel::error:
        return "error";
    }
    return "info";
}

domain::Result<LogLevel> parse_log_level(std::string_view text) {
    for (const auto level :
         {LogLevel::trace, LogLevel::debug, LogLevel::info, LogLevel::warn, LogLevel::error}) {
        if (to_string(level) == text) {
            return level;
        }
    }
    return domain::Error::validation("logging.level", "must be trace, debug, info, warn or error");
}

std::string format_utc(std::chrono::system_clock::time_point time) {
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    auto seconds = static_cast<std::time_t>(millis / 1000);
    auto remainder = millis % 1000;
    if (remainder < 0) {
        remainder += 1000;
        seconds -= 1;
    }
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    // Large enough for any int field values, so the output can never be truncated.
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                  static_cast<int>(remainder));
    return buffer;
}

void JsonLineSink::write(const LogRecord& record) {
    nlohmann::json line;
    line["time"] = format_utc(record.time);
    line["level"] = to_string(record.level);
    line["event"] = record.event;
    for (const auto& field : record.fields) {
        if (field.key != "time" && field.key != "level" && field.key != "event") {
            line[field.key] = to_json(field.value);
        }
    }
    // Invalid UTF-8 from upstream must not break the log line.
    const auto text = line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    const std::lock_guard lock{mutex_};
    out_ << text << '\n';
    out_.flush();
}

void TextSink::write(const LogRecord& record) {
    std::string line = format_utc(record.time);
    line.push_back(' ');
    for (const char c : to_string(record.level)) {
        line.push_back(static_cast<char>(c - 'a' + 'A'));
    }
    line.push_back(' ');
    line.append(escape_text(record.event));
    for (const auto& field : record.fields) {
        line.append(" ").append(escape_text(field.key)).append("=").append(
            escape_text(to_text(field.value)));
    }
    const std::lock_guard lock{mutex_};
    out_ << line << '\n';
    out_.flush();
}

void MemorySink::write(const LogRecord& record) {
    const std::lock_guard lock{mutex_};
    records_.push_back(record);
}

std::vector<LogRecord> MemorySink::records() const {
    const std::lock_guard lock{mutex_};
    return records_;
}

RotatingFileSink::RotatingFileSink(std::filesystem::path file, bool text_format, LogRotation options)
    : file_(std::move(file)), text_(text_format), options_(options) {
    if (options_.max_file_bytes == 0 || options_.retained_files > 20)
        throw std::invalid_argument("invalid log rotation limits");
    open();
}

void RotatingFileSink::open() {
    stream_.close();
    stream_.clear();
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(file_, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) throw std::runtime_error("cannot inspect log file");
    if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))
        throw std::runtime_error("log path must be a regular file");
    bytes_ = std::filesystem::exists(status) ? std::filesystem::file_size(file_) : 0;
    stream_.open(file_, std::ios::app | std::ios::binary);
    if (!stream_) throw std::runtime_error("cannot open log file");
}

std::filesystem::path RotatingFileSink::archive(std::uint32_t index) const {
    auto path = file_;
    path += "." + std::to_string(index);
    return path;
}

void RotatingFileSink::rotate() {
    stream_.close();
    // Only these exact numbered files are retained or removed; never enumerate/delete a directory.
    const auto remove_file = [](const std::filesystem::path& path) {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(path, ec);
        if (ec == std::errc::no_such_file_or_directory) return;
        if (ec) throw std::runtime_error("cannot inspect log archive");
        if (!std::filesystem::exists(status)) return;
        if (!std::filesystem::is_regular_file(status)) throw std::runtime_error("log archive must be a regular file");
        std::filesystem::remove(path);
    };
    if (options_.retained_files == 0) {
        remove_file(file_);
    } else {
        remove_file(archive(options_.retained_files));
        for (auto index = options_.retained_files; index > 1; --index) {
            const auto previous = archive(index - 1);
            if (std::filesystem::exists(previous)) {
                if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(previous)))
                    throw std::runtime_error("log archive must be a regular file");
                std::filesystem::rename(previous, archive(index));
            }
        }
        if (std::filesystem::exists(file_)) std::filesystem::rename(file_, archive(1));
    }
    open();
}

void RotatingFileSink::write(const LogRecord& record) {
    std::ostringstream buffer;
    if (text_) TextSink{buffer}.write(record);
    else JsonLineSink{buffer}.write(record);
    const auto line = buffer.str();
    if (line.size() > options_.max_file_bytes) throw std::runtime_error("log record exceeds file limit");
    const std::lock_guard lock{mutex_};
    if (!stream_.is_open() || !stream_) open();
    if (bytes_ > options_.max_file_bytes - line.size()) rotate();
    stream_.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream_.flush();
    if (!stream_) throw std::runtime_error("log write failed");
    bytes_ += line.size();
}

Logger::Logger(std::shared_ptr<LogSink> sink, LogLevel minimum,
               std::shared_ptr<const application::ports::Clock> clock)
    : sink_(std::move(sink)), minimum_(minimum), clock_(std::move(clock)) {}

Logger Logger::with(std::initializer_list<LogField> fields) const {
    Logger child = *this;
    for (const auto& field : fields) {
        child.bound_.push_back(redact(field));
    }
    return child;
}

void Logger::log(LogLevel level, std::string_view event,
                 std::initializer_list<LogField> fields) const noexcept {
    if (!enabled(level) || !sink_) {
        return;
    }
    try {
        LogRecord record{clock_->now(), level, std::string{event}, bound_};
        record.fields.reserve(bound_.size() + fields.size());
        for (const auto& field : fields) record.fields.push_back(redact(field));
        sink_->write(record);
    } catch (...) {} // logging failures must never change a business operation's outcome
}

namespace {
// Even a permanently blocked OS output cannot cause unbounded writer threads.
std::atomic<unsigned> log_workers{0};
constexpr unsigned max_log_workers = 4;

class OwnedStderrSink final : public LogSink {
public:
    explicit OwnedStderrSink(bool text_format) : text_(text_format) {
#ifdef _WIN32
        const auto original = _fileno(stderr);
        if (original >= 0) fd_ = _dup(original);
#else
        fd_ = dup(STDERR_FILENO);
#endif
    }
    ~OwnedStderrSink() override {
        if (fd_ >= 0) {
#ifdef _WIN32
            _close(fd_);
#else
            ::close(fd_);
#endif
        }
    }
    void write(const LogRecord& record) override {
        if (fd_ < 0) throw std::runtime_error("stderr is unavailable");
        std::ostringstream buffer;
        if (text_) TextSink{buffer}.write(record);
        else JsonLineSink{buffer}.write(record);
        auto line = buffer.str();
        std::size_t offset = 0;
        while (offset < line.size()) {
#ifdef _WIN32
            const auto count = _write(fd_, line.data() + offset, static_cast<unsigned>(line.size() - offset));
#else
            const auto count = ::write(fd_, line.data() + offset, line.size() - offset);
#endif
            if (count <= 0) throw std::runtime_error("stderr write failed");
            offset += static_cast<std::size_t>(count);
        }
    }
private:
    int fd_{-1};
    bool text_;
};

std::size_t record_bytes(const LogRecord& record) {
    auto bytes = sizeof(record) + record.event.size() + 1 + record.fields.size() * sizeof(LogField);
    for (const auto& field : record.fields) {
        bytes += field.key.size() + 1;
        if (const auto* value = std::get_if<std::string>(&field.value)) bytes += value->size() + 1;
    }
    return bytes;
}
} // namespace

struct AsyncLogSink::State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::pair<LogRecord, std::size_t>> queue;
    std::shared_ptr<LogSink> target;
    Stats stats;
    std::size_t capacity, max_bytes;
    bool closing{false}, done{false};
};

AsyncLogSink::AsyncLogSink(std::shared_ptr<LogSink> target, std::size_t capacity, std::size_t max_bytes,
                           std::chrono::milliseconds drain_budget)
    : state_(std::make_shared<State>()), drain_budget_(drain_budget) {
    auto state = state_;
    state->target = std::move(target);
    state->capacity = capacity;
    state->max_bytes = max_bytes;
    if (!state->target || log_workers.fetch_add(1) >= max_log_workers) {
        if (state->target) --log_workers;
        state->closing = state->done = true;
        return;
    }
    try {
        worker_ = std::thread{[state] {
#ifndef _WIN32
            // A closed pipe is an output error, not a process-wide SIGPIPE.
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &mask, nullptr);
#endif
            for (;;) {
                std::pair<LogRecord, std::size_t> item;
                {
                    std::unique_lock lock{state->mutex};
                    state->changed.wait(lock, [&] { return state->closing || !state->queue.empty(); });
                    if (state->queue.empty()) break;
                    item = std::move(state->queue.front());
                    state->queue.pop_front();
                    --state->stats.queued;
                    // Retain the byte charge while the output owns the record.
                }
                bool failed = false;
                try { state->target->write(item.first); } catch (...) { failed = true; }
                {
                    const std::lock_guard lock{state->mutex};
                    state->stats.bytes -= item.second;
                    if (failed) ++state->stats.failures;
                    else ++state->stats.written;
                }
            }
            {
                const std::lock_guard lock{state->mutex};
                state->done = true;
            }
            --log_workers;
            state->changed.notify_all();
        }};
    } catch (...) { --log_workers; throw; }
}

AsyncLogSink::~AsyncLogSink() { close(); }

void AsyncLogSink::write(const LogRecord& record) {
    const auto bytes = record_bytes(record);
    const std::lock_guard lock{state_->mutex};
    if (state_->closing || state_->queue.size() >= state_->capacity || bytes > state_->max_bytes - state_->stats.bytes) {
        ++state_->stats.dropped;
        return;
    }
    try {
        state_->queue.emplace_back(record, bytes);
        ++state_->stats.queued;
        state_->stats.bytes += bytes;
    } catch (...) { ++state_->stats.dropped; }
    state_->changed.notify_one();
}

AsyncLogSink::Stats AsyncLogSink::stats() const {
    const std::lock_guard lock{state_->mutex};
    return state_->stats;
}

void AsyncLogSink::close() noexcept {
    if (!worker_.joinable()) return;
    bool done;
    {
        std::unique_lock lock{state_->mutex};
        state_->closing = true;
        state_->changed.notify_all();
        done = state_->changed.wait_for(lock, drain_budget_, [&] { return state_->done; });
        if (!done) {
            for (const auto& item : state_->queue) state_->stats.bytes -= item.second;
            state_->stats.dropped += state_->queue.size();
            state_->stats.queued = 0;
            state_->queue.clear();
        }
    }
    if (done) worker_.join();
    else worker_.detach(); // shared State and owned fd outlive this facade; bounded by max_log_workers
}

std::shared_ptr<AsyncLogSink> make_async_stderr_sink(bool text_format) {
    return std::make_shared<AsyncLogSink>(std::make_shared<OwnedStderrSink>(text_format));
}

} // namespace music_service::infrastructure::observability
