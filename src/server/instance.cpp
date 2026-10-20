#include "server/instance.hpp"

#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/storage/sqlite/migrations.hpp"

#include <set>
#include <utility>

namespace music_service::server {

namespace fs = std::filesystem;
namespace sqlite = infrastructure::storage::sqlite;

void Instance::close() noexcept {
    identities.reset();
    credentials.reset();
    destinations.reset();
    tasks.reset();
    database.reset();
    lock.release();
}

domain::Result<std::unique_ptr<Instance>> open_instance(const application::config::ServiceConfig& config) {
    auto layout = infrastructure::filesystem::prepare_data_dir(config.data_dir);
    if (!layout) {
        return std::move(layout).error();
    }
    auto lock = infrastructure::filesystem::InstanceLock::acquire(layout->lock_file);
    if (!lock) {
        return std::move(lock).error();
    }
    std::unique_ptr<Instance> instance{new Instance{std::move(layout).value(), std::move(lock).value(),
                                                    std::make_shared<infrastructure::SystemClock>(),
                                                    std::make_shared<infrastructure::observability::IdGenerator>(),
                                                    nullptr, nullptr, nullptr, nullptr, nullptr}};
    sqlite::DatabaseOptions options;
    options.read_connections = config.storage.read_connections;
    options.writer_queue = config.storage.writer_queue;
    options.wait_budget = config.storage.wait_budget;
    auto database = sqlite::Database::open(instance->layout.database, options);
    if (!database) {
        return std::move(database).error();
    }
    instance->database = std::move(database).value();
    if (auto migrated = sqlite::migrate(*instance->database, instance->clock->now()); !migrated) {
        return std::move(migrated).error();
    }
    const sqlite::RepositoryContext context{instance->database.get(), instance->clock, instance->ids};
    instance->identities = std::make_shared<sqlite::SqliteIdentityRepository>(context);
    instance->credentials = std::make_shared<sqlite::SqliteCredentialRepository>(context);
    instance->destinations = std::make_shared<sqlite::SqliteDestinationRepository>(context);
    instance->tasks = std::make_shared<sqlite::SqliteTaskRepository>(
        context, sqlite::TaskRepositoryOptions{config.download_limits.per_principal_active,
                                               config.download_limits.global_active, config.idempotency.retention});
    return instance;
}

domain::Result<std::vector<DestinationState>> register_destinations(Instance& instance,
                                                                    const application::config::ServiceConfig& config) {
    std::vector<DestinationState> states;
    std::set<std::string> configured;
    for (const auto& destination : config.destinations) {
        configured.insert(destination.id);
        auto bound = instance.destinations->get(destination.id);
        if (!bound && bound.error().code != domain::ErrorCode::not_found) {
            return std::move(bound).error();
        }
        auto checked = infrastructure::filesystem::check_destination_root(destination.root, instance.layout.root);
        if (checked) {
            auto registered = instance.destinations->bind(destination.id,
                                                          infrastructure::filesystem::path_to_utf8(checked.value()));
            if (!registered) {
                auto error = std::move(registered).error();
                error.details.push_back({"destinations/" + destination.id, "root differs from the bound root"});
                return error;
            }
            states.push_back({destination, checked.value(), true});
            continue;
        }
        if (!bound) {
            // A new id must be registered with a usable root.
            auto error = std::move(checked).error();
            error.details.push_back({"destinations/" + destination.id, "cannot be registered"});
            return error;
        }
        // Bound but unusable now (offline share, permissions): the configured root
        // must still be the bound one; it is never replaced by another directory.
        const auto configured_root = infrastructure::filesystem::path_to_utf8(fs::weakly_canonical(destination.root));
        if (configured_root != bound->canonical_root) {
            domain::Error error{domain::ErrorCode::destination_root_changed,
                                "destination root differs from the bound root", false, {}};
            error.details.push_back({"destinations/" + destination.id, "root differs from the bound root"});
            return error;
        }
        states.push_back({destination, infrastructure::filesystem::path_from_utf8(bound->canonical_root), false});
    }

    auto bindings = instance.destinations->list();
    if (!bindings) {
        return std::move(bindings).error();
    }
    for (const auto& binding : bindings.value()) {
        if (configured.count(binding.id) != 0) {
            continue;
        }
        auto used = instance.destinations->in_use(binding.id);
        if (!used) {
            return std::move(used).error();
        }
        if (used.value()) {
            domain::Error error{domain::ErrorCode::destination_in_use,
                                "a destination still in use was removed from the configuration", false, {}};
            error.details.push_back({"destinations/" + binding.id, "is referenced by tasks, files or intents"});
            return error;
        }
    }
    return states;
}

} // namespace music_service::server
