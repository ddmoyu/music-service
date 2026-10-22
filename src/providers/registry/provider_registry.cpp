#include "providers/registry/provider_registry.hpp"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <random>
#include <set>
#include <utility>

namespace music_service::providers {

namespace {

using domain::Error;
using domain::ErrorCode;
using domain::ErrorDetail;

// The ports a capability key can be bound to. verify_credentials is served by auth
// profile verifiers instead and has no entry here.
const std::map<std::string_view, std::type_index>& capability_ports() {
    static const std::map<std::string_view, std::type_index> ports_by_key{
        {ports::capability_traits<ports::ITrackSearch>::key, typeid(ports::ITrackSearch)},
        {ports::capability_traits<ports::ITrackDetail>::key, typeid(ports::ITrackDetail)},
        {ports::capability_traits<ports::ILinkParser>::key, typeid(ports::ILinkParser)},
        {ports::capability_traits<ports::IAssetResolver>::key, typeid(ports::IAssetResolver)},
        {ports::capability_traits<ports::ILyrics>::key, typeid(ports::ILyrics)},
        {ports::capability_traits<ports::PlaylistSearch>::key, typeid(ports::PlaylistSearch)},
        {ports::capability_traits<ports::AlbumSearch>::key, typeid(ports::AlbumSearch)},
        {ports::capability_traits<ports::ArtistSearch>::key, typeid(ports::ArtistSearch)},
        {ports::capability_traits<ports::PlaylistDetail>::key, typeid(ports::PlaylistDetail)},
        {ports::capability_traits<ports::AlbumDetail>::key, typeid(ports::AlbumDetail)},
        {ports::capability_traits<ports::ChartDetail>::key, typeid(ports::ChartDetail)},
        {ports::capability_traits<ports::PlaylistTracks>::key, typeid(ports::PlaylistTracks)},
        {ports::capability_traits<ports::AlbumTracks>::key, typeid(ports::AlbumTracks)},
        {ports::capability_traits<ports::ArtistTracks>::key, typeid(ports::ArtistTracks)},
        {ports::capability_traits<ports::ChartTracks>::key, typeid(ports::ChartTracks)},
        {ports::capability_traits<ports::ArtistAlbums>::key, typeid(ports::ArtistAlbums)},
        {ports::capability_traits<ports::CategoryPlaylists>::key, typeid(ports::CategoryPlaylists)},
        {ports::capability_traits<ports::ICategories>::key, typeid(ports::ICategories)},
        {ports::capability_traits<ports::ICharts>::key, typeid(ports::ICharts)},
        {ports::capability_traits<ports::IRecommendedPlaylists>::key, typeid(ports::IRecommendedPlaylists)},
    };
    return ports_by_key;
}

class Problems {
public:
    void add(std::string field, std::string reason) {
        details_.push_back({std::move(field), std::move(reason)});
    }
    [[nodiscard]] bool empty() const noexcept { return details_.empty(); }
    [[nodiscard]] Error error() && {
        return Error{ErrorCode::validation_failed, "provider registration failed", false,
                     std::move(details_)};
    }

private:
    std::vector<ErrorDetail> details_;
};

std::string interface_name(std::type_index type) {
    for (const auto& [key, port] : capability_ports()) {
        if (port == type) {
            return "the " + std::string{key} + " port";
        }
    }
    if (type == std::type_index{typeid(ports::ICredentialVerifier)}) {
        return "the credential verifier port";
    }
    return "an unknown port";
}

void check_names(const std::vector<std::string>& names, const std::string& field, Problems& p) {
    std::set<std::string_view> seen;
    for (const auto& name : names) {
        if (name.empty()) {
            p.add(field, "must not contain empty names");
        } else if (!seen.insert(name).second) {
            p.add(field, "lists " + name + " twice");
        }
    }
}

void check_auth_profiles(const ports::ProviderBundle& bundle, const std::string& base,
                         Problems& p) {
    std::set<std::string_view> seen;
    for (const auto& profile : bundle.auth_profiles) {
        const auto field = base + "/auth_profiles/" + profile.auth_profile;
        if (!domain::SourceId::parse(profile.auth_profile)) {
            p.add(base + "/auth_profiles", "auth_profile " + profile.auth_profile +
                                               " must match [a-z][a-z0-9_]{0,63}");
            continue;
        }
        if (!seen.insert(profile.auth_profile).second) {
            p.add(field, "is registered twice");
        }
        if (profile.version == 0) {
            p.add(field + "/version", "must be at least 1");
        }
        std::set<std::string_view> fields;
        for (const auto& input : profile.input_fields) {
            if (input.name.empty() || !fields.insert(input.name).second) {
                p.add(field + "/input_fields", "names must be non-empty and unique");
                break;
            }
        }
        check_names(profile.scopes, field + "/scopes", p);
    }
}

const ports::AuthProfileSpec* find_profile(const ports::ProviderBundle& bundle,
                                           std::string_view name) {
    const auto it = std::find_if(bundle.auth_profiles.begin(), bundle.auth_profiles.end(),
                                 [&](const auto& profile) { return profile.auth_profile == name; });
    return it == bundle.auth_profiles.end() ? nullptr : &*it;
}

void check_capabilities(const ports::ProviderBundle& bundle, const std::string& base,
                        Problems& p) {
    const auto verify_key = std::string{ports::capability::verify_credentials};
    std::set<std::string_view> declared;
    for (const auto& spec : bundle.capabilities) {
        const auto field = base + "/capabilities/" + spec.key;
        if (!declared.insert(spec.key).second) {
            p.add(field, "is declared twice");
            continue;
        }
        const bool verify = spec.key == verify_key;
        if (!verify && capability_ports().find(spec.key) == capability_ports().end()) {
            p.add(field, "has no port in this build");
        }
        if (spec.credentials == ports::CredentialRequirement::none) {
            if (!spec.auth_profiles.empty()) {
                p.add(field + "/auth_profiles", "must be empty when no credentials are used");
            }
        } else if (spec.auth_profiles.empty()) {
            p.add(field + "/auth_profiles", "must name the profiles the capability can use");
        }
        if (verify && spec.credentials != ports::CredentialRequirement::required) {
            p.add(field + "/credentials", "verify_credentials always requires credentials");
        }
        check_names(spec.auth_profiles, field + "/auth_profiles", p);
        check_names(spec.scopes, field + "/scopes", p);
        check_names(spec.regions, field + "/regions", p);
        check_names(spec.dependencies, field + "/dependencies", p);
        for (const auto& name : spec.auth_profiles) {
            const auto* profile = find_profile(bundle, name);
            if (profile == nullptr) {
                p.add(field + "/auth_profiles", "names unregistered profile " + name);
            } else if (verify && !profile->verifier) {
                p.add(field + "/auth_profiles", "profile " + name + " has no verifier");
            }
        }
    }

    // Every verifier must be reachable through a declared verify_credentials.
    const auto verify_spec =
        std::find_if(bundle.capabilities.begin(), bundle.capabilities.end(),
                     [&](const auto& spec) { return spec.key == verify_key; });
    for (const auto& profile : bundle.auth_profiles) {
        if (!profile.verifier) {
            continue;
        }
        const bool listed = verify_spec != bundle.capabilities.end() &&
                            std::find(verify_spec->auth_profiles.begin(),
                                      verify_spec->auth_profiles.end(),
                                      profile.auth_profile) != verify_spec->auth_profiles.end();
        if (!listed) {
            p.add(base + "/auth_profiles/" + profile.auth_profile + "/verifier",
                  "is bound but verify_credentials does not declare the profile");
        }
    }

    std::set<std::string_view> bound;
    for (const auto& binding : bundle.bindings) {
        const auto field = base + "/bindings/" + binding.key();
        if (!bound.insert(binding.key()).second) {
            p.add(field, "is bound twice");
            continue;
        }
        if (declared.find(binding.key()) == declared.end()) {
            p.add(field, "is bound but not declared");
        }
        if (!binding.has_factory()) {
            p.add(field, "has an empty factory");
        }
        const auto port = capability_ports().find(binding.key());
        if (port == capability_ports().end()) {
            if (binding.key() == verify_key) {
                p.add(field, "verify_credentials is bound through auth profile verifiers");
            }
            // Other unknown keys are already reported by the declaration check, or by
            // "bound but not declared".
        } else if (port->second != binding.interface_type()) {
            p.add(field, "factory produces " + interface_name(binding.interface_type()) +
                             ", expected " + interface_name(port->second));
        }
    }
    for (const auto& spec : bundle.capabilities) {
        if (spec.key != verify_key && bound.find(spec.key) == bound.end()) {
            p.add(base + "/capabilities/" + spec.key, "is declared without a factory");
        }
    }
}

void check_plaintext_endpoints(const ports::ProviderBundle& bundle, const std::string& base, Problems& p) {
    for (const auto& endpoint : bundle.plaintext_endpoints) {
        const auto lowercase = std::none_of(endpoint.host.begin(), endpoint.host.end(),
                                            [](char c) { return c >= 'A' && c <= 'Z'; });
        if (endpoint.host.empty() || !lowercase || endpoint.port == 0 || endpoint.path_prefix.empty() ||
            endpoint.path_prefix.front() != '/' || endpoint.operation.empty()) {
            p.add(base + "/plaintext_endpoints",
                  "each needs a lowercase host, a port, a path prefix starting with / and an operation");
            return;
        }
    }
}

void check_bundle(const ports::ProviderBundle& bundle,
                  const application::config::ProviderConfig& config, Problems& p) {
    const auto base = "providers/" + bundle.source.value();
    if (bundle.contract_version != ports::provider_contract_version) {
        p.add(base + "/contract_version",
              "is " + std::to_string(bundle.contract_version) + ", this build supports " +
                  std::to_string(ports::provider_contract_version));
        return; // the rest of an incompatible bundle cannot be interpreted
    }
    if (bundle.name.empty()) {
        p.add(base + "/name", "must not be empty");
    }
    if (bundle.implementation_id.empty()) {
        p.add(base + "/implementation_id", "must not be empty");
    }
    if (bundle.implementation_version.empty()) {
        p.add(base + "/implementation_version", "must not be empty");
    }
    if (config.implementation != ports::to_string(bundle.execution_mode)) {
        p.add(base + "/implementation", "is " + config.implementation + ", but this build provides " +
                                            std::string{ports::to_string(bundle.execution_mode)});
    }
    if (bundle.validate_options) {
        if (auto valid = bundle.validate_options(config.options_json); !valid) {
            if (valid.error().details.empty()) {
                p.add(base + "/options", valid.error().message);
            }
            for (const auto& detail : valid.error().details) {
                p.add(base + "/options" + (detail.field.empty() ? "" : "/" + detail.field),
                      detail.reason);
            }
        }
    } else if (config.options_json != "{}") {
        p.add(base + "/options", "this provider accepts no options");
    }
    check_auth_profiles(bundle, base, p);
    check_capabilities(bundle, base, p);
    check_plaintext_endpoints(bundle, base, p);
}

std::uint64_t fnv1a64(std::string_view data) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : data) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void append_field(std::string& out, std::string_view value) {
    out.append(std::to_string(value.size())).push_back(':');
    out.append(value);
}

// Binds a cursor to the principal and the filter, not to the page size.
std::uint64_t list_fingerprint(const ProviderListQuery& query) {
    std::string canonical;
    append_field(canonical, query.principal_id);
    append_field(canonical, query.capability ? "c" + *query.capability : "-");
    append_field(canonical, query.enabled ? (*query.enabled ? "1" : "0") : "-");
    return fnv1a64(canonical);
}

std::string hex16(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[value & 0xF];
        value >>= 4;
    }
    return out;
}

std::optional<std::uint64_t> parse_hex16(std::string_view text) {
    std::uint64_t value = 0;
    if (text.size() != 16) {
        return std::nullopt;
    }
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

constexpr std::string_view cursor_prefix = "pv1.";

struct ListCursor {
    std::uint64_t generation;
    std::uint64_t fingerprint;
    std::string after;
};

std::optional<ListCursor> parse_cursor(std::string_view text) {
    if (text.substr(0, cursor_prefix.size()) != cursor_prefix) {
        return std::nullopt;
    }
    text.remove_prefix(cursor_prefix.size());
    // <generation>.<fingerprint>.<source>
    if (text.size() < 34 || text[16] != '.' || text[33] != '.') {
        return std::nullopt;
    }
    const auto generation = parse_hex16(text.substr(0, 16));
    const auto fingerprint = parse_hex16(text.substr(17, 16));
    const auto after = text.substr(34);
    if (!generation || !fingerprint || !domain::SourceId::parse(after)) {
        return std::nullopt;
    }
    return ListCursor{*generation, *fingerprint, std::string{after}};
}

Error cursor_error(ErrorCode code, std::string reason) {
    Error error{code, code == ErrorCode::cursor_expired ? "cursor has expired" : "cursor does not match",
                false, {}};
    error.details.push_back({"cursor", std::move(reason)});
    return error;
}

std::uint64_t random_generation() {
    std::random_device device;
    std::uint64_t value = 0;
    while (value == 0) {
        value = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    }
    return value;
}

} // namespace

// Lazily created object, guarded by its own mutex so one slow factory does not block
// other capabilities.
struct Slot {
    std::mutex mutex;
    std::shared_ptr<void> instance;
};

struct ProviderRegistry::Entry {
    ProviderView view;
    ports::ProviderSetup setup;
    std::vector<ports::PlaintextEndpoint> plaintext_endpoints;
    std::map<std::string, std::pair<ports::CapabilityBinding, std::unique_ptr<Slot>>, std::less<>>
        bindings;
    struct Profile {
        ports::AuthProfileSpec spec;
        std::unique_ptr<Slot> slot;
    };
    std::map<std::string, Profile, std::less<>> profiles;
};

ProviderRegistry::ProviderRegistry() = default;
ProviderRegistry::~ProviderRegistry() = default;

domain::Result<std::shared_ptr<const ProviderRegistry>>
ProviderRegistry::build(std::vector<ports::ProviderBundle> bundles,
                        const std::map<std::string, application::config::ProviderConfig>& config,
                        ProviderServices services) {
    Problems problems;
    std::set<std::string_view> seen;
    for (const auto& bundle : bundles) {
        if (!seen.insert(bundle.source.value()).second) {
            problems.add("providers/" + bundle.source.value(), "source is registered twice");
        }
    }
    for (const auto& [source, ignored] : config) {
        if (seen.find(source) == seen.end()) {
            problems.add("providers/" + source, "is configured but not built into this binary");
        }
    }
    const application::config::ProviderConfig defaults;
    for (const auto& bundle : bundles) {
        const auto it = config.find(bundle.source.value());
        check_bundle(bundle, it == config.end() ? defaults : it->second, problems);
    }
    if (!problems.empty()) {
        return std::move(problems).error();
    }

    std::shared_ptr<ProviderRegistry> registry{new ProviderRegistry()};
    registry->generation_ = random_generation();
    for (auto& bundle : bundles) {
        const auto it = config.find(bundle.source.value());
        const auto& settings = it == config.end() ? defaults : it->second;

        ProviderView view{bundle.source,
                          bundle.name,
                          settings.enabled,
                          bundle.implementation_id,
                          bundle.implementation_version,
                          bundle.execution_mode,
                          {},
                          {},
                          bundle.quality_hints};
        for (auto& spec : bundle.capabilities) {
            view.capabilities.push_back({std::move(spec), settings.enabled});
        }
        std::sort(view.capabilities.begin(), view.capabilities.end(),
                  [](const auto& a, const auto& b) { return a.spec.key < b.spec.key; });
        for (const auto& profile : bundle.auth_profiles) {
            view.auth_methods.push_back({profile.auth_profile, profile.kind, profile.version,
                                         profile.input_fields, profile.scopes,
                                         static_cast<bool>(profile.verifier), false, false});
        }
        std::sort(view.auth_methods.begin(), view.auth_methods.end(),
                  [](const auto& a, const auto& b) { return a.auth_profile < b.auth_profile; });

        auto entry = std::unique_ptr<Entry>(
            new Entry{std::move(view),
                      ports::ProviderSetup{bundle.source, settings.options_json, services.upstream,
                                           settings.rate_limit_group.value_or(""), services.helper},
                      std::move(bundle.plaintext_endpoints), {}, {}});
        for (auto& binding : bundle.bindings) {
            auto key = binding.key();
            entry->bindings.emplace(std::move(key),
                                    std::pair{std::move(binding), std::make_unique<Slot>()});
        }
        for (auto& profile : bundle.auth_profiles) {
            auto key = profile.auth_profile;
            entry->profiles.emplace(std::move(key),
                                    Entry::Profile{std::move(profile), std::make_unique<Slot>()});
        }
        auto source = bundle.source.value();
        registry->entries_.emplace(std::move(source), std::move(entry));
    }
    return std::shared_ptr<const ProviderRegistry>{std::move(registry)};
}

domain::Result<ProviderView> ProviderRegistry::describe(std::string_view source) const {
    const auto it = entries_.find(source);
    if (it == entries_.end()) {
        return Error{ErrorCode::unknown_provider, "source is not registered", false, {}};
    }
    return it->second->view;
}

domain::Result<ProviderListPage> ProviderRegistry::list(const ProviderListQuery& query) const {
    if (query.limit == 0 || query.limit > max_list_limit) {
        return Error::validation("limit", "must be between 1 and 100");
    }
    const auto fingerprint = list_fingerprint(query);
    auto it = entries_.begin();
    if (query.cursor) {
        const auto cursor = parse_cursor(*query.cursor);
        if (!cursor) {
            return cursor_error(ErrorCode::cursor_mismatch, "is not a providers cursor");
        }
        if (cursor->generation != generation_) {
            return cursor_error(ErrorCode::cursor_expired, "was issued before the service restarted");
        }
        if (cursor->fingerprint != fingerprint) {
            return cursor_error(ErrorCode::cursor_mismatch, "belongs to another caller or filter");
        }
        it = entries_.upper_bound(cursor->after);
    }

    const auto matches = [&](const Entry& entry) {
        return (!query.capability || entry.view.supports(*query.capability)) &&
               (!query.enabled || entry.view.enabled == *query.enabled);
    };
    ProviderListPage page;
    for (; it != entries_.end(); ++it) {
        if (!matches(*it->second)) {
            continue;
        }
        if (page.items.size() == query.limit) {
            const auto& last = page.items.back().source.value();
            page.next_cursor = std::string{cursor_prefix} + hex16(generation_) + "." +
                               hex16(fingerprint) + "." + last;
            break;
        }
        page.items.push_back(it->second->view);
    }
    return page;
}

std::vector<domain::SourceId> ProviderRegistry::sources() const {
    std::vector<domain::SourceId> out;
    out.reserve(entries_.size());
    for (const auto& [ignored, entry] : entries_) {
        out.push_back(entry->view.source);
    }
    return out;
}

std::size_t ProviderRegistry::size() const noexcept { return entries_.size(); }

std::vector<ports::PlaintextEndpoint> ProviderRegistry::plaintext_endpoints(std::string_view source) const {
    const auto it = entries_.find(source);
    return it == entries_.end() ? std::vector<ports::PlaintextEndpoint>{} : it->second->plaintext_endpoints;
}

domain::Result<void> ProviderRegistry::check_source(std::string_view source) const {
    if (entries_.find(source) == entries_.end()) {
        return Error{ErrorCode::unknown_provider, "source is not registered", false, {}};
    }
    return domain::ok();
}

domain::Result<ports::AuthProfileInfo>
ProviderRegistry::describe_auth_profile(std::string_view source, std::string_view auth_profile) const {
    const auto it = entries_.find(source);
    if (it == entries_.end()) {
        return Error{ErrorCode::unknown_provider, "source is not registered", false, {}};
    }
    const auto profile = it->second->profiles.find(auth_profile);
    if (profile == it->second->profiles.end()) {
        return Error{ErrorCode::capability_unsupported, "auth profile is not registered", false, {}};
    }
    const auto& spec = profile->second.spec;
    return ports::AuthProfileInfo{spec.auth_profile, spec.kind, spec.version, spec.input_fields,
                                  static_cast<bool>(spec.verifier)};
}

domain::Result<ports::CapabilitySpec> ProviderRegistry::describe_capability(std::string_view source,
                                                                           std::string_view key) const {
    auto entry = enabled_entry(source);
    if (!entry) {
        return std::move(entry).error();
    }
    const auto& capabilities = entry.value()->view.capabilities;
    const auto it = std::find_if(capabilities.begin(), capabilities.end(),
                                 [&](const CapabilityView& capability) { return capability.spec.key == key; });
    if (it == capabilities.end()) {
        return Error{ErrorCode::capability_unsupported, "source does not support " + std::string{key},
                     false, {}};
    }
    return it->spec;
}

std::size_t ProviderRegistry::instantiated() const noexcept { return instantiated_.load(); }

std::uint64_t ProviderRegistry::generation() const noexcept { return generation_; }

domain::Result<const ProviderRegistry::Entry*>
ProviderRegistry::enabled_entry(std::string_view source) const {
    const auto it = entries_.find(source);
    if (it == entries_.end()) {
        return Error{ErrorCode::unknown_provider, "source is not registered", false, {}};
    }
    if (!it->second->view.enabled) {
        return Error{ErrorCode::provider_disabled, "source is disabled by configuration", false, {}};
    }
    return it->second.get();
}

namespace {

// Returns the slot's object, creating it under the slot's mutex on first use. A failed
// factory leaves the slot empty so the next call tries again.
template <class Make>
domain::Result<std::shared_ptr<void>> get_or_create(Slot& slot, std::atomic<std::size_t>& created,
                                                    Make&& make) {
    std::scoped_lock lock{slot.mutex};
    if (!slot.instance) {
        auto made = make();
        if (!made) {
            return std::move(made).error();
        }
        if (!made.value()) {
            return Error{ErrorCode::internal, "provider factory returned no object", false, {}};
        }
        slot.instance = std::move(made).value();
        ++created;
    }
    return slot.instance;
}

} // namespace

domain::Result<std::shared_ptr<void>>
ProviderRegistry::require_erased(std::string_view source, std::string_view key,
                                 std::type_index type) const {
    auto entry = enabled_entry(source);
    if (!entry) {
        return std::move(entry).error();
    }
    const auto it = entry.value()->bindings.find(key);
    if (it == entry.value()->bindings.end()) {
        return Error{ErrorCode::capability_unsupported,
                     "source does not support " + std::string{key}, false, {}};
    }
    const auto& [binding, slot] = it->second;
    if (binding.interface_type() != type) {
        // Unreachable for require<Interface>() since keys and ports are checked at build.
        return Error{ErrorCode::internal, "capability is bound to another port", false, {}};
    }
    return get_or_create(*slot, instantiated_,
                         [&, setup = &entry.value()->setup] { return binding.create(*setup); });
}

domain::Result<std::shared_ptr<ports::ICredentialVerifier>>
ProviderRegistry::require_verifier(std::string_view source, std::string_view auth_profile,
                                   ports::CredentialKind kind) const {
    auto entry = enabled_entry(source);
    if (!entry) {
        return std::move(entry).error();
    }
    const auto it = entry.value()->profiles.find(auth_profile);
    if (it == entry.value()->profiles.end() || it->second.spec.kind != kind ||
        !it->second.spec.verifier) {
        return Error{ErrorCode::capability_unsupported,
                     "source cannot verify this auth profile and kind", false, {}};
    }
    const auto& profile = it->second;
    auto made = get_or_create(*profile.slot, instantiated_, [&]() -> domain::Result<std::shared_ptr<void>> {
        auto verifier = profile.spec.verifier(entry.value()->setup);
        if (!verifier) {
            return std::move(verifier).error();
        }
        return std::shared_ptr<void>{std::move(verifier).value()};
    });
    if (!made) {
        return std::move(made).error();
    }
    return std::static_pointer_cast<ports::ICredentialVerifier>(std::move(made).value());
}

} // namespace music_service::providers
