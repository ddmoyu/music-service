#pragma once

#include "application/ports/runtime/clock.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace music_service::infrastructure::observability {

enum class LogLevel { trace, debug, info, warn, error };

std::string_view to_string(LogLevel level) noexcept;
domain::Result<LogLevel> parse_log_level(std::string_view text);

using LogValue = std::variant<std::string, std::int64_t, std::uint64_t, double, bool>;

struct LogField {
    std::string key;
    LogValue value;

    LogField(std::string k, std::string v) : key(std::move(k)), value(std::move(v)) {}
    LogField(std::string k, const char* v) : key(std::move(k)), value(std::string{v}) {}
    LogField(std::string k, std::string_view v) : key(std::move(k)), value(std::string{v}) {}
    LogField(std::string k, std::int64_t v) : key(std::move(k)), value(v) {}
    LogField(std::string k, int v) : key(std::move(k)), value(static_cast<std::int64_t>(v)) {}
    LogField(std::string k, std::uint64_t v) : key(std::move(k)), value(v) {}
    LogField(std::string k, std::uint32_t v)
        : key(std::move(k)), value(static_cast<std::uint64_t>(v)) {}
    LogField(std::string k, double v) : key(std::move(k)), value(v) {}
    LogField(std::string k, bool v) : key(std::move(k)), value(v) {}
};

struct LogRecord {
    std::chrono::system_clock::time_point time;
    LogLevel level{LogLevel::info};
    std::string event;
    std::vector<LogField> fields; // already redacted
};

// Implementations must be safe to call from several threads.
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
};

// One JSON object per line: {"time":"...Z","level":"info","event":"...",<fields>}.
class JsonLineSink final : public LogSink {
public:
    explicit JsonLineSink(std::ostream& out) : out_(out) {}
    void write(const LogRecord& record) override;

private:
    std::mutex mutex_;
    std::ostream& out_;
};

// Human-readable: "<time> <LEVEL> <event> key=value ...".
class TextSink final : public LogSink {
public:
    explicit TextSink(std::ostream& out) : out_(out) {}
    void write(const LogRecord& record) override;

private:
    std::mutex mutex_;
    std::ostream& out_;
};

// Keeps records in memory; for tests.
class MemorySink final : public LogSink {
public:
    void write(const LogRecord& record) override;
    [[nodiscard]] std::vector<LogRecord> records() const;

private:
    mutable std::mutex mutex_;
    std::vector<LogRecord> records_;
};

// Structured logger. Every string field passes redact_field before reaching a sink, so
// tokens, cookies, tickets and signed URL parameters cannot be written by accident.
// with() returns a child that adds bound fields such as request_id or task_id.
class Logger {
public:
    Logger(std::shared_ptr<LogSink> sink, LogLevel minimum,
           std::shared_ptr<const application::ports::Clock> clock);

    [[nodiscard]] Logger with(std::initializer_list<LogField> fields) const;
    [[nodiscard]] bool enabled(LogLevel level) const noexcept { return level >= minimum_; }

    void log(LogLevel level, std::string_view event,
             std::initializer_list<LogField> fields = {}) const;
    void info(std::string_view event, std::initializer_list<LogField> fields = {}) const {
        log(LogLevel::info, event, fields);
    }
    void warn(std::string_view event, std::initializer_list<LogField> fields = {}) const {
        log(LogLevel::warn, event, fields);
    }
    void error(std::string_view event, std::initializer_list<LogField> fields = {}) const {
        log(LogLevel::error, event, fields);
    }

private:
    std::shared_ptr<LogSink> sink_;
    LogLevel minimum_;
    std::shared_ptr<const application::ports::Clock> clock_;
    std::vector<LogField> bound_;
};

// UTC RFC 3339 with milliseconds, e.g. 2026-10-19T08:00:00.000Z.
std::string format_utc(std::chrono::system_clock::time_point time);

} // namespace music_service::infrastructure::observability
