#pragma once

// A minimal SSE protocol client for the events listener, and the client merge rule.

#include "service_harness.hpp"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

namespace music_service::test {

struct Frame {
    std::string event; // empty for a comment
    std::string data;
    std::string comment;
};

// Reads one SSE stream on its own thread and parses it as a client must: messages end
// at a blank line, wherever the network split the bytes.
class SseClient {
public:
    SseClient(int port, const std::string& token, httplib::Headers headers = {}) {
        thread_ = std::thread([this, port, token, headers = std::move(headers)] {
            httplib::Client client("127.0.0.1", port);
            client.set_connection_timeout(3s);
            client.set_read_timeout(20s);
            if (!token.empty()) {
                client.set_bearer_token_auth(token);
            }
            auto result = client.Get(
                "/events/v1/downloads", headers,
                [this](const httplib::Response& response) {
                    const std::lock_guard lock{mutex_};
                    status_ = response.status;
                    content_type_ = response.get_header_value("Content-Type");
                    retry_after_ = response.get_header_value("Retry-After");
                    return true;
                },
                [this](const char* data, std::size_t size) {
                    const std::lock_guard lock{mutex_};
                    if (status_ != 200) {
                        body_.append(data, size); // a JSON error before any stream
                        return true;
                    }
                    raw_.append(data, size);
                    parse();
                    changed_.notify_all();
                    return !stop_;
                });
            const std::lock_guard lock{mutex_};
            if (result) {
                status_ = result->status;
            }
            ended_ = true;
            changed_.notify_all();
        });
    }
    ~SseClient() { close(); }
    SseClient(const SseClient&) = delete;
    SseClient& operator=(const SseClient&) = delete;

    // The client side of a disconnect: takes effect at the next bytes (a heartbeat).
    void close() {
        {
            const std::lock_guard lock{mutex_};
            stop_ = true;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // The next event, skipping heartbeats; nullopt after the timeout or the end.
    std::optional<Frame> next_event(std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock{mutex_};
        const auto until = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            while (!frames_.empty()) {
                auto frame = std::move(frames_.front());
                frames_.pop_front();
                if (!frame.event.empty()) {
                    return frame;
                }
                ++heartbeats_;
            }
            if (ended_ || !changed_.wait_until(lock, until, [this] { return ended_ || !frames_.empty(); })) {
                if (frames_.empty()) {
                    return std::nullopt;
                }
            }
        }
    }

    bool wait_end(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return ended_; });
    }
    int status() {
        std::unique_lock lock{mutex_};
        changed_.wait_for(lock, 5s, [this] { return status_ != 0 || ended_; });
        return status_;
    }
    std::string body() {
        wait_end(5s);
        const std::lock_guard lock{mutex_};
        return body_;
    }
    std::string content_type() {
        const std::lock_guard lock{mutex_};
        return content_type_;
    }
    std::string retry_after() {
        const std::lock_guard lock{mutex_};
        return retry_after_;
    }
    int heartbeats() {
        const std::lock_guard lock{mutex_};
        for (const auto& frame : frames_) {
            heartbeats_ += frame.event.empty() ? 1 : 0;
        }
        std::erase_if(frames_, [](const Frame& frame) { return frame.event.empty(); });
        return heartbeats_;
    }

private:
    void parse() {
        for (auto end = raw_.find("\n\n"); end != std::string::npos; end = raw_.find("\n\n")) {
            const auto block = raw_.substr(0, end);
            raw_.erase(0, end + 2);
            Frame frame;
            std::size_t start = 0;
            while (start <= block.size()) {
                auto stop = block.find('\n', start);
                if (stop == std::string::npos) {
                    stop = block.size();
                }
                const auto line = block.substr(start, stop - start);
                if (line.rfind("event: ", 0) == 0) {
                    frame.event = line.substr(7);
                } else if (line.rfind("data: ", 0) == 0) {
                    frame.data += line.substr(6);
                } else if (line.rfind(":", 0) == 0) {
                    frame.comment = line.substr(1);
                }
                start = stop + 1;
            }
            frames_.push_back(std::move(frame));
        }
    }

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string raw_;
    std::deque<Frame> frames_;
    int status_{0};
    int heartbeats_{0};
    std::string content_type_;
    std::string retry_after_;
    std::string body_;
    bool ended_{false};
    bool stop_{false};
};

// The client merge rule (api-contract.md 9.4): per task, only a newer version replaces.
struct TaskBoard {
    std::map<std::string, std::pair<std::uint64_t, std::string>> tasks;
    bool apply(const json& task) {
        const auto id = task["task_id"].get<std::string>();
        const auto version = task["version"].get<std::uint64_t>();
        auto& entry = tasks[id];
        if (version <= entry.first) {
            return false;
        }
        entry = {version, task["status"].get<std::string>()};
        return true;
    }
};

} // namespace music_service::test
