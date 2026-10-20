#include "infrastructure/storage/sqlite/database.hpp"

#include <algorithm>
#include <string>

namespace music_service::infrastructure::storage::sqlite {

namespace {

domain::Error busy(std::string_view what) {
    return domain::Error{domain::ErrorCode::storage_unavailable,
                         std::string{"database "} + std::string{what} + " is busy", true, {}};
}

domain::Result<void> configure(Connection& connection, std::chrono::milliseconds wait_budget,
                               bool writer) {
    const std::string busy_timeout =
        "PRAGMA busy_timeout = " + std::to_string(wait_budget.count()) + ";";
    if (auto result = connection.exec(busy_timeout); !result) {
        return result;
    }
    if (writer) {
        // WAL with FULL sync: a commit that returned is durable across power loss, which
        // the API relies on before answering 202 for a new task.
        return connection.exec("PRAGMA journal_mode = WAL;"
                               "PRAGMA synchronous = FULL;"
                               "PRAGMA foreign_keys = ON;"
                               "PRAGMA trusted_schema = OFF;");
    }
    return connection.exec("PRAGMA query_only = ON;"
                           "PRAGMA trusted_schema = OFF;");
}

} // namespace

Database::WriterLease::~WriterLease() {
    if (owner_ != nullptr) {
        owner_->writer_mutex_.unlock();
    }
}

Database::ReaderLease::~ReaderLease() {
    if (owner_ != nullptr) {
        {
            const std::lock_guard lock{owner_->readers_mutex_};
            owner_->idle_readers_.push_back(connection);
        }
        owner_->readers_available_.notify_one();
    }
}

Database::Database(std::filesystem::path file, DatabaseOptions options, Connection writer,
                   std::vector<Connection> readers)
    : file_(std::move(file)), options_(options),
      writer_(std::make_unique<Connection>(std::move(writer))) {
    for (auto& reader : readers) {
        readers_.push_back(std::make_unique<Connection>(std::move(reader)));
        idle_readers_.push_back(readers_.back().get());
    }
}

domain::Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path& file,
                                                         DatabaseOptions options) {
    auto writer = Connection::open(file, Connection::Mode::read_write_create);
    if (!writer) {
        return std::move(writer).error();
    }
    if (auto configured = configure(writer.value(), options.wait_budget, true); !configured) {
        return configured.error();
    }

    std::vector<Connection> readers;
    for (std::uint32_t i = 0; i < std::max<std::uint32_t>(options.read_connections, 1); ++i) {
        // Read-write handle restricted by query_only: read-only opens of a WAL database
        // can fail before the -shm file exists.
        auto reader = Connection::open(file, Connection::Mode::read_write_create);
        if (!reader) {
            return std::move(reader).error();
        }
        if (auto configured = configure(reader.value(), options.wait_budget, false); !configured) {
            return configured.error();
        }
        readers.push_back(std::move(reader).value());
    }
    return std::unique_ptr<Database>{
        new Database{file, options, std::move(writer).value(), std::move(readers)}};
}

domain::Result<Database::WriterLease> Database::acquire_writer() {
    // Bounded queue: beyond writer_queue waiters, fail fast instead of piling up threads.
    if (writer_waiters_.fetch_add(1) >= options_.writer_queue) {
        writer_waiters_.fetch_sub(1);
        return busy("writer queue");
    }
    const bool locked = writer_mutex_.try_lock_for(options_.wait_budget);
    writer_waiters_.fetch_sub(1);
    if (!locked) {
        return busy("writer");
    }
    return WriterLease{this};
}

domain::Result<Database::ReaderLease> Database::acquire_reader() {
    std::unique_lock lock{readers_mutex_};
    if (!readers_available_.wait_for(lock, options_.wait_budget,
                                     [this] { return !idle_readers_.empty(); })) {
        return busy("reader pool");
    }
    auto* connection = idle_readers_.back();
    idle_readers_.pop_back();
    return ReaderLease{this, connection};
}

} // namespace music_service::infrastructure::storage::sqlite
