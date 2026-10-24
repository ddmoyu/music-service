#pragma once

#include "infrastructure/storage/sqlite/connection.hpp"

#include "domain/result.hpp"
#include "application/ports/observability/metrics.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace music_service::infrastructure::storage::sqlite {

struct DatabaseOptions {
    std::uint32_t read_connections{2};
    std::uint32_t writer_queue{128};
    std::chrono::milliseconds wait_budget{500};
    std::shared_ptr<application::ports::Metrics> metrics{};
};

// Single SQLite database in WAL mode with exactly one writer and a small pool of
// read-only connections (data-models.md, SQLite 表设计).
// - write(): waits at most wait_budget for the writer (and refuses at once when
//   writer_queue callers are already waiting), runs the function inside
//   BEGIN IMMEDIATE, commits on success and rolls back on any error.
// - read(): runs the function on a read-only connection inside a read transaction,
//   so it sees one consistent snapshot.
// Functions passed in must not wait for network, file processing or other workers.
class Database {
public:
    static domain::Result<std::unique_ptr<Database>> open(const std::filesystem::path& file,
                                                          DatabaseOptions options);

    template <class F> auto write(F&& function) -> std::invoke_result_t<F, Connection&> {
        application::ports::MetricTimer timer{options_.metrics, application::ports::MetricOperation::storage_write};
        auto lease = acquire_writer();
        if (!lease) {
            return std::move(lease).error();
        }
        auto result = in_transaction(*writer_, "BEGIN IMMEDIATE", std::forward<F>(function));
        timer.outcome = result ? application::ports::MetricOutcome::success : application::ports::metric_outcome(result.error());
        return result;
    }

    template <class F> auto read(F&& function) -> std::invoke_result_t<F, Connection&> {
        application::ports::MetricTimer timer{options_.metrics, application::ports::MetricOperation::storage_read};
        auto lease = acquire_reader();
        if (!lease) {
            return std::move(lease).error();
        }
        auto result = in_transaction(*lease.value().connection, "BEGIN", std::forward<F>(function));
        timer.outcome = result ? application::ports::MetricOutcome::success : application::ports::metric_outcome(result.error());
        return result;
    }

    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

private:
    class WriterLease {
    public:
        explicit WriterLease(Database* owner) : owner_(owner) {}
        WriterLease(WriterLease&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
        WriterLease(const WriterLease&) = delete;
        WriterLease& operator=(const WriterLease&) = delete;
        WriterLease& operator=(WriterLease&&) = delete;
        ~WriterLease();

    private:
        Database* owner_;
    };

    class ReaderLease {
    public:
        ReaderLease(Database* owner, Connection* leased)
            : connection(leased), owner_(owner) {}
        ReaderLease(ReaderLease&& other) noexcept
            : connection(other.connection), owner_(std::exchange(other.owner_, nullptr)) {}
        ReaderLease(const ReaderLease&) = delete;
        ReaderLease& operator=(const ReaderLease&) = delete;
        ReaderLease& operator=(ReaderLease&&) = delete;
        ~ReaderLease();

        Connection* connection;

    private:
        Database* owner_;
    };

    Database(std::filesystem::path file, DatabaseOptions options, Connection writer,
             std::vector<Connection> readers);

    domain::Result<WriterLease> acquire_writer();
    domain::Result<ReaderLease> acquire_reader();

    template <class F>
    static auto in_transaction(Connection& connection, const char* begin, F&& function)
        -> std::invoke_result_t<F, Connection&> {
        if (auto started = connection.exec(begin); !started) {
            return started.error();
        }
        // Rolls back on every exit that is not a successful commit, exceptions included,
        // so the connection never stays inside a transaction.
        RollbackGuard guard{connection};
        auto result = std::forward<F>(function)(connection);
        if (!result) {
            return result;
        }
        if (auto committed = connection.exec("COMMIT"); !committed) {
            return committed.error();
        }
        guard.release();
        return result;
    }

    class RollbackGuard {
    public:
        explicit RollbackGuard(Connection& connection) : connection_(&connection) {}
        RollbackGuard(const RollbackGuard&) = delete;
        RollbackGuard& operator=(const RollbackGuard&) = delete;
        ~RollbackGuard() {
            if (connection_ == nullptr) {
                return;
            }
            try {
                (void)connection_->exec("ROLLBACK");
            } catch (...) {
            }
        }
        void release() noexcept { connection_ = nullptr; }

    private:
        Connection* connection_;
    };

    std::filesystem::path file_;
    DatabaseOptions options_;

    std::timed_mutex writer_mutex_;
    std::atomic<std::uint32_t> writer_waiters_{0};
    std::unique_ptr<Connection> writer_;

    std::mutex readers_mutex_;
    std::condition_variable readers_available_;
    std::vector<std::unique_ptr<Connection>> readers_;
    std::vector<Connection*> idle_readers_;
};

} // namespace music_service::infrastructure::storage::sqlite
