#include "infrastructure/observability/logger.hpp"

#include "infrastructure/observability/redaction.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <ctime>
#include <type_traits>
#include <utility>

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
    line.append(record.event);
    for (const auto& field : record.fields) {
        line.append(" ").append(field.key).append("=").append(to_text(field.value));
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
                 std::initializer_list<LogField> fields) const {
    if (!enabled(level) || !sink_) {
        return;
    }
    LogRecord record{clock_->now(), level, std::string{event}, bound_};
    record.fields.reserve(bound_.size() + fields.size());
    for (const auto& field : fields) {
        record.fields.push_back(redact(field));
    }
    sink_->write(record);
}

} // namespace music_service::infrastructure::observability
