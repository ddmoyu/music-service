#include "server/commands.hpp"

#include "application/accounts/identity_admin.hpp"
#include "infrastructure/auth/access_tokens.hpp"
#include "infrastructure/config/config_loader.hpp"
#include "infrastructure/filesystem/data_layout.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/observability/logger.hpp"
#include "infrastructure/filesystem/instance_lock.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"
#include "infrastructure/storage/sqlite/backup.hpp"
#include "infrastructure/storage/sqlite/connection.hpp"
#include "providers/registry/builtin_bundles.hpp"
#include "server/instance.hpp"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
#include <httplib.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace music_service::server {

namespace {

namespace fs = std::filesystem;
namespace cfg = application::config;
namespace ports = application::ports;
using infrastructure::filesystem::path_from_utf8;
using infrastructure::filesystem::path_to_utf8;

struct Arguments {
    std::optional<fs::path> config_file;
    std::optional<fs::path> data_dir;
    std::vector<std::string> words;               // command words
    std::map<std::string, std::string> options;   // --name value
};

void print_error(const Console& console, const domain::Error& error) {
    console.err << "error: " << domain::to_string(error.code) << ": " << error.message << '\n';
    for (const auto& detail : error.details) {
        console.err << "  " << detail.field << ": " << detail.reason << '\n';
    }
    if (error.code == domain::ErrorCode::instance_locked) {
        console.err << "  the service (or another command) is using this data_dir; stop it first\n";
    }
}

std::optional<Arguments> parse(const std::vector<std::string>& args, const Console& console) {
    Arguments parsed;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg.rfind("--", 0) == 0) {
            if (i + 1 >= args.size()) {
                console.err << "error: " << arg << " needs a value\n";
                return std::nullopt;
            }
            const auto& value = args[++i];
            if (arg == "--config") {
                parsed.config_file = path_from_utf8(value);
            } else if (arg == "--data-dir") {
                parsed.data_dir = path_from_utf8(value);
            } else if (!parsed.options.emplace(arg.substr(2), value).second) {
                console.err << "error: " << arg << " given twice\n";
                return std::nullopt;
            }
        } else {
            parsed.words.push_back(arg);
        }
    }
    return parsed;
}

void usage(std::ostream& out) {
    out << "usage: music-service [--config FILE] [--data-dir DIR] [COMMAND]\n"
           "  serve                                  run the service (default)\n"
           "  version | check-config | init | healthcheck\n"
           "  principal create --name NAME | principal list\n"
           "  principal enable --id ID | principal disable --id ID\n"
           "  token issue --principal ID --scopes A,B --output FILE\n"
           "  token list --principal ID | token revoke --id ID\n"
           "  backup --output FILE                   consistent database copy, also while serving\n"
           "  restore --input FILE                   replace the database (service stopped)\n";
}

std::set<std::string, std::less<>> provider_sources(const HostOptions& host) {
    if (!host.bundles) {
        return providers::builtin_sources();
    }
    std::set<std::string, std::less<>> sources;
    for (const auto& bundle : *host.bundles) {
        sources.insert(bundle.source.value());
    }
    return sources;
}

domain::Result<cfg::ServiceConfig> load_config(const Arguments& args, const HostOptions& host) {
    infrastructure::config::LoadOptions options;
    options.config_file = args.config_file;
    options.data_dir = args.data_dir;
    options.working_directory = fs::current_path();
    options.build.providers = provider_sources(host);
    options.build.event_server = true;
    options.build.media_server = true;
    options.build.media_proxy = true;
    options.build.direct_media_urls = true;
    return infrastructure::config::load_service_config(options);
}

// Fails with exit_usage when a required option is missing.
std::optional<std::string> required(const Arguments& args, const std::string& name, const Console& console) {
    const auto it = args.options.find(name);
    if (it == args.options.end() || it->second.empty()) {
        console.err << "error: --" << name << " is required\n";
        return std::nullopt;
    }
    return it->second;
}

bool only_options(const Arguments& args, const std::vector<std::string_view>& allowed, const Console& console) {
    for (const auto& [name, value] : args.options) {
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            console.err << "error: unknown option --" << name << '\n';
            return false;
        }
    }
    return true;
}

std::vector<std::string> split_scopes(const std::string& text) {
    std::vector<std::string> scopes;
    std::stringstream in{text};
    for (std::string scope; std::getline(in, scope, ',');) {
        scopes.push_back(scope);
    }
    return scopes;
}

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        out += (out.empty() ? "" : ",") + value;
    }
    return out;
}

void print_principal(const Console& console, const ports::PrincipalRecord& p) {
    console.out << p.id << '\t' << p.display_name << '\t' << (p.enabled ? "enabled" : "disabled") << '\n';
}

void print_token(const Console& console, const ports::AccessTokenRecord& t) {
    console.out << t.id << '\t' << t.principal_id << '\t' << join(t.scopes) << '\t'
                << infrastructure::observability::format_utc(t.created_at) << '\t'
                << (t.revoked_at ? "revoked " + infrastructure::observability::format_utc(*t.revoked_at) : "active")
                << '\n';
}

application::accounts::IdentityAdminService identity_admin(Instance& instance) {
    return application::accounts::IdentityAdminService{instance.identities,
                                                       std::make_shared<infrastructure::auth::MbedTlsTokenCodec>(),
                                                       std::make_shared<infrastructure::auth::FileTokenDelivery>()};
}

void print_issued(const Console& console, const application::accounts::IssuedToken& issued) {
    // The location only; the plaintext stays in the file.
    console.out << "token " << issued.token.id << " for " << issued.token.principal_id << " ("
                << join(issued.token.scopes) << ") written to " << path_to_utf8(issued.delivered_to) << '\n';
}

int cmd_init(const cfg::ServiceConfig& config, const Console& console) {
    auto instance = open_instance(config);
    if (!instance) {
        print_error(console, instance.error());
        return exit_failed;
    }
    if (auto destinations = register_destinations(**instance, config); !destinations) {
        print_error(console, destinations.error());
        return exit_failed;
    }
    auto admin = identity_admin(**instance);
    auto issued = admin.initialize((*instance)->layout.bootstrap_token);
    if (!issued) {
        print_error(console, issued.error());
        return exit_failed;
    }
    console.out << "initialized " << path_to_utf8((*instance)->layout.root) << '\n';
    print_issued(console, issued.value());
    console.out << "move the token to the client, then delete the file\n";
    return exit_ok;
}

// Online: a consistent copy while the service may be running, so no instance lock.
int cmd_backup(const Arguments& args, const cfg::ServiceConfig& config, const Console& console) {
    if (!only_options(args, {"output"}, console)) {
        return exit_usage;
    }
    const auto output = required(args, "output", console);
    if (!output) {
        return exit_usage;
    }
    const auto layout = infrastructure::filesystem::DataLayout::under(config.data_dir);
    const auto target = fs::absolute(path_from_utf8(*output));
    auto saved = infrastructure::storage::sqlite::backup_database(layout.database, target);
    if (!saved) {
        print_error(console, saved.error());
        return exit_failed;
    }
    console.out << "backup " << path_to_utf8(target) << " schema_version=" << saved->schema_version
                << " bytes=" << saved->size_bytes << '\n';
    console.out << "secrets/ is not part of the backup; keep a copy of it with the database\n";
    return exit_ok;
}

// Offline: holds the instance lock, so the service must be stopped.
int cmd_restore(const Arguments& args, const cfg::ServiceConfig& config, const Console& console) {
    if (!only_options(args, {"input"}, console)) {
        return exit_usage;
    }
    const auto input = required(args, "input", console);
    if (!input) {
        return exit_usage;
    }
    auto layout = infrastructure::filesystem::prepare_data_dir(config.data_dir);
    if (!layout) {
        print_error(console, layout.error());
        return exit_failed;
    }
    auto lock = infrastructure::filesystem::InstanceLock::acquire(layout->lock_file);
    if (!lock) {
        print_error(console, lock.error());
        return exit_failed;
    }
    std::error_code ec;
    const bool had_database = fs::exists(layout->database, ec);
    const auto replaced = new_backup_path(layout->backups, "replaced", std::chrono::system_clock::now());
    const auto source = fs::absolute(path_from_utf8(*input));
    auto restored = infrastructure::storage::sqlite::restore_database(source, layout->database, replaced);
    if (!restored) {
        print_error(console, restored.error());
        return exit_failed;
    }
    console.out << "restored " << path_to_utf8(layout->database) << " from " << path_to_utf8(source)
                << " schema_version=" << restored->schema_version << '\n';
    if (had_database) {
        console.out << "the replaced database is kept at " << path_to_utf8(replaced) << '\n';
    }
    console.out << "secrets/ was not changed; restore it from the same backup if credentials are missing\n";
    return exit_ok;
}

int cmd_principal(const Arguments& args, const cfg::ServiceConfig& config, const Console& console) {
    const auto action = args.words.size() > 1 ? args.words[1] : std::string{};
    if (action != "create" && action != "list" && action != "enable" && action != "disable") {
        usage(console.err);
        return exit_usage;
    }
    const std::vector<std::string_view> allowed = action == "create" ? std::vector<std::string_view>{"name"}
                                                  : action == "list"   ? std::vector<std::string_view>{}
                                                                       : std::vector<std::string_view>{"id"};
    if (!only_options(args, allowed, console)) {
        return exit_usage;
    }
    std::optional<std::string> value;
    if (action != "list") {
        value = required(args, action == "create" ? "name" : "id", console);
        if (!value) {
            return exit_usage;
        }
    }
    auto instance = open_instance(config);
    if (!instance) {
        print_error(console, instance.error());
        return exit_failed;
    }
    auto admin = identity_admin(**instance);
    if (action == "list") {
        auto listed = admin.list_principals();
        if (!listed) {
            print_error(console, listed.error());
            return exit_failed;
        }
        for (const auto& p : listed.value()) {
            print_principal(console, p);
        }
        return exit_ok;
    }
    auto result = action == "create" ? admin.create_principal(*value)
                                     : admin.set_principal_enabled(*value, action == "enable");
    if (!result) {
        print_error(console, result.error());
        return exit_failed;
    }
    print_principal(console, result.value());
    return exit_ok;
}

int cmd_token(const Arguments& args, const cfg::ServiceConfig& config, const Console& console) {
    const auto action = args.words.size() > 1 ? args.words[1] : std::string{};
    std::vector<std::string_view> allowed;
    if (action == "issue") {
        allowed = {"principal", "scopes", "output"};
    } else if (action == "list") {
        allowed = {"principal"};
    } else if (action == "revoke") {
        allowed = {"id"};
    } else {
        usage(console.err);
        return exit_usage;
    }
    if (!only_options(args, allowed, console)) {
        return exit_usage;
    }
    std::vector<std::string> values;
    for (const auto name : allowed) {
        auto value = required(args, std::string{name}, console);
        if (!value) {
            return exit_usage;
        }
        values.push_back(std::move(*value));
    }
    auto instance = open_instance(config);
    if (!instance) {
        print_error(console, instance.error());
        return exit_failed;
    }
    auto admin = identity_admin(**instance);
    if (action == "issue") {
        auto output = fs::absolute(path_from_utf8(values[2]));
        auto issued = admin.issue_token(values[0], split_scopes(values[1]), output);
        if (!issued) {
            print_error(console, issued.error());
            return exit_failed;
        }
        print_issued(console, issued.value());
        return exit_ok;
    }
    if (action == "list") {
        auto listed = admin.list_tokens(values[0]);
        if (!listed) {
            print_error(console, listed.error());
            return exit_failed;
        }
        for (const auto& t : listed.value()) {
            print_token(console, t);
        }
        return exit_ok;
    }
    auto revoked = admin.revoke_token(values[0]);
    if (!revoked) {
        print_error(console, revoked.error());
        return exit_failed;
    }
    print_token(console, revoked.value());
    return exit_ok;
}

// Validates configuration, build capabilities, paths and, when a database exists,
// the destination bindings, reading it without changing anything. It refuses what
// serve would refuse: a changed root, or a removed destination still in use.
int cmd_check_config(const cfg::ServiceConfig& config, const Console& console) {
    const auto data_dir = fs::weakly_canonical(config.data_dir);
    bool ok = true;
    std::map<std::string, std::string> roots;
    for (const auto& d : config.destinations) {
        auto checked = infrastructure::filesystem::check_destination_root(d.root, data_dir);
        if (!checked) {
            console.err << "destination " << d.id << ": ";
            print_error(console, checked.error());
            ok = false;
            continue;
        }
        roots[d.id] = path_to_utf8(checked.value());
    }
    const auto database = infrastructure::filesystem::DataLayout::under(data_dir).database;
    std::error_code ec;
    if (fs::exists(database, ec)) {
        namespace sqlite = infrastructure::storage::sqlite;
        auto connection = sqlite::Connection::open(database, sqlite::Connection::Mode::read_only);
        auto statement = connection ? connection->prepare("SELECT id, canonical_root FROM destinations")
                                    : domain::Result<sqlite::Statement>{connection.error()};
        if (!statement) {
            print_error(console, statement.error());
            return exit_failed;
        }
        std::vector<std::string> removed;
        while (true) {
            auto row = statement->step();
            if (!row) {
                print_error(console, row.error());
                return exit_failed;
            }
            if (!row.value()) {
                break;
            }
            const auto id = statement->text(0);
            const bool configured = std::any_of(config.destinations.begin(), config.destinations.end(),
                                                [&](const auto& d) { return d.id == id; });
            if (!configured) {
                removed.push_back(id);
                continue;
            }
            const auto it = roots.find(id);
            if (it != roots.end() && it->second != statement->text(1)) {
                console.err << "error: DESTINATION_ROOT_CHANGED: destination " << id << " is bound to "
                            << statement->text(1) << '\n';
                ok = false;
            }
        }
        for (const auto& id : removed) {
            auto used = sqlite::destination_in_use(connection.value(), id);
            if (!used) {
                print_error(console, used.error());
                return exit_failed;
            }
            if (used.value()) {
                console.err << "error: DESTINATION_IN_USE: destination " << id
                            << " was removed from the configuration but tasks, files or intents still use it\n";
                ok = false;
            }
        }
    }
    if (!ok) {
        return exit_failed;
    }
    console.out << "configuration is valid\n";
    return exit_ok;
}

int cmd_healthcheck(const cfg::ServiceConfig& config, const Console& console) {
    auto host = config.control.host;
    if (host == "0.0.0.0") {
        host = "127.0.0.1";
    } else if (host == "::") {
        host = "::1";
    }
    httplib::Client client(host, config.control.port);
    client.set_connection_timeout(std::chrono::seconds{3});
    client.set_read_timeout(std::chrono::seconds{3});
    auto response = client.Get("/health/ready");
    if (!response) {
        console.err << "not ready: " << httplib::to_string(response.error()) << '\n';
        return exit_failed;
    }
    if (response->status != 200) {
        console.err << "not ready: HTTP " << response->status << '\n';
        return exit_failed;
    }
    console.out << "ready\n";
    return exit_ok;
}

int cmd_serve(const cfg::ServiceConfig& config, const Console& console, CommandHooks hooks) {
    auto host = ProcessHost::start(config, std::move(hooks.host));
    if (!host) {
        print_error(console, host.error());
        return exit_failed;
    }
    const auto& report = host.value()->report();
    // One line on stdout for launchers; logs go to stderr.
    nlohmann::json line{{"event", "ready"},
                        {"instance_id", report.instance_id},
                        {"api_base_url", report.api_base_url},
                        {"media_base_url", report.media_base_url ? nlohmann::json(*report.media_base_url) : nullptr},
                        {"events_base_url", report.events_base_url ? nlohmann::json(*report.events_base_url) : nullptr}};
    console.out << line.dump() << std::endl;
    if (hooks.on_ready) {
        hooks.on_ready(report);
    }
    host.value()->run_until(hooks.should_stop ? hooks.should_stop : [] { return false; });
    return exit_ok;
}

} // namespace

int run_command(const std::vector<std::string>& args, Console console, CommandHooks hooks) {
    auto parsed = parse(args, console);
    if (!parsed) {
        usage(console.err);
        return exit_usage;
    }
    const auto command = parsed->words.empty() ? std::string{"serve"} : parsed->words.front();
    if (command == "version") {
        console.out << "music-service " << service_version << '\n';
        return exit_ok;
    }
    if (command == "help") {
        usage(console.out);
        return exit_ok;
    }
    const bool known = command == "serve" || command == "check-config" || command == "init" ||
                       command == "healthcheck" || command == "principal" || command == "token" ||
                       command == "backup" || command == "restore";
    if (!known) {
        console.err << "error: unknown command " << command << '\n';
        usage(console.err);
        return exit_usage;
    }
    if (command != "principal" && command != "token") {
        const bool takes_options = command == "backup" || command == "restore";
        if (parsed->words.size() > 1 || (!takes_options && !parsed->options.empty())) {
            usage(console.err);
            return exit_usage;
        }
    }
    auto config = load_config(*parsed, hooks.host);
    if (!config) {
        print_error(console, config.error());
        return exit_usage;
    }
    if (command == "check-config") {
        return cmd_check_config(config.value(), console);
    }
    if (command == "init") {
        return cmd_init(config.value(), console);
    }
    if (command == "healthcheck") {
        return cmd_healthcheck(config.value(), console);
    }
    if (command == "principal") {
        return cmd_principal(*parsed, config.value(), console);
    }
    if (command == "token") {
        return cmd_token(*parsed, config.value(), console);
    }
    if (command == "backup") {
        return cmd_backup(*parsed, config.value(), console);
    }
    if (command == "restore") {
        return cmd_restore(*parsed, config.value(), console);
    }
    return cmd_serve(config.value(), console, std::move(hooks));
}

} // namespace music_service::server
