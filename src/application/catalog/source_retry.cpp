#include "application/catalog/source_retry.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>

namespace music_service::application::catalog {
namespace {
using domain::Error;
using domain::ErrorCode;
using json = nlohmann::json;

// 只收敛 ASCII 空白，保留大小写、标点、括号和所有版本文字，不做模糊匹配。
std::string normalized(std::string_view text) {
    std::string result;
    bool space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = !result.empty();
        } else {
            if (space) result += ' ';
            result += c;
            space = false;
        }
    }
    return result;
}

std::vector<std::string> artists(const domain::Track& track) {
    std::vector<std::string> names;
    if (track.artists.empty() || track.artists.size() > 32) return {};
    for (const auto& artist : track.artists) {
        auto name = normalized(artist.name);
        if (name.empty() || name.size() > 512) return {};
        names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());
    // 重复姓名代表上游身份信息不明确，不猜测合唱名单。
    if (std::adjacent_find(names.begin(), names.end()) != names.end()) return {};
    return names;
}

std::optional<Error> stopped(const ports::OperationContext& operation) {
    if (operation.stop.stop_requested()) return Error{ErrorCode::canceled, "source retry canceled", false, {}};
    if (operation.expired(std::chrono::steady_clock::now()))
        return Error{ErrorCode::deadline_exceeded, "source retry time budget exhausted", true, {}};
    return {};
}

Error exhausted(const std::vector<std::string>& attempted) {
    Error result{ErrorCode::source_exhausted, "no untried source has a strictly matching track", false,
                 {{"match_policy", "exact title, complete artist list and version; duration within 2000 ms when known"}}};
    for (const auto& source : attempted) result.details.push_back({"attempted_sources", source});
    return result;
}
} // namespace

bool strict_track_match(const domain::Track& a, const domain::Track& b) {
    const auto title = normalized(a.title);
    const auto names = artists(a);
    if (title.empty() || title.size() > 1024 || names.empty() || title != normalized(b.title) || names != artists(b))
        return false;
    const auto ordinary = [](domain::VersionKind kind) {
        return kind == domain::VersionKind::unknown || kind == domain::VersionKind::studio;
    };
    if (a.version.kind != b.version.kind && (!ordinary(a.version.kind) || !ordinary(b.version.kind))) return false;
    if (a.version.label.value_or("").size() > 512 || b.version.label.value_or("").size() > 512 ||
        normalized(a.version.label.value_or("")) != normalized(b.version.label.value_or(""))) return false;
    if (a.duration_ms && b.duration_ms) {
        if (*a.duration_ms <= 0 || *b.duration_ms <= 0) return false;
        if (std::max(*a.duration_ms, *b.duration_ms) - std::min(*a.duration_ms, *b.duration_ms) > 2000) return false;
    }
    return true;
}

bool source_failure(ErrorCode code) {
    switch (code) {
    case ErrorCode::upstream_failed: case ErrorCode::upstream_timeout: case ErrorCode::upstream_rate_limited:
    case ErrorCode::resource_expired: case ErrorCode::credentials_required: case ErrorCode::quality_unavailable:
    case ErrorCode::preview_rejected: case ErrorCode::audio_invalid: case ErrorCode::not_found:
        return true;
    default: return false;
    }
}

std::string encode_source_retry(const SourceRetryState& state) {
    json original = nullptr;
    if (state.original) {
        const auto& t = *state.original;
        std::vector<std::string> names;
        for (const auto& a : t.artists) names.push_back(a.name);
        original = {{"title", t.title}, {"artists", names},
                    {"duration_ms", t.duration_ms ? json(*t.duration_ms) : json(nullptr)},
                    {"version_kind", static_cast<int>(t.version.kind)},
                    {"version_label", t.version.label ? json(*t.version.label) : json(nullptr)}};
    }
    return json{{"schema", 1}, {"match_policy", "strict"}, {"original_identity", original},
                {"attempted_sources", state.attempted_sources}, {"pending", state.pending}}.dump();
}

domain::Result<SourceRetryState> decode_source_retry(const std::optional<std::string>& execution,
                                                    const domain::TrackRef& original) {
    SourceRetryState state;
    if (!execution) return state;
    try {
        const auto doc = json::parse(*execution);
        if (!doc.is_object()) throw std::invalid_argument("execution");
        if (!doc.contains("source_retry")) return state;
        const auto& s = doc.at("source_retry");
        if (s.at("schema") != 1 || s.at("match_policy") != "strict") throw std::invalid_argument("schema");
        state.pending = s.at("pending").get<bool>();
        state.attempted_sources = s.at("attempted_sources").get<std::vector<std::string>>();
        std::set<std::string> seen;
        if (state.attempted_sources.size() > SourceRetryService::max_sources) throw std::invalid_argument("sources");
        for (const auto& source : state.attempted_sources)
            if (!domain::SourceId::parse(source) || !seen.insert(source).second) throw std::invalid_argument("source");
        const auto& t = s.at("original_identity");
        if (!t.is_null()) {
            domain::Track track{original, t.at("title").get<std::string>(), {}, {}, {}, {}};
            for (const auto& name : t.at("artists").get<std::vector<std::string>>()) track.artists.push_back({name, {}});
            if (!t.at("duration_ms").is_null()) track.duration_ms = t.at("duration_ms").get<std::int64_t>();
            const auto kind = t.at("version_kind").get<int>();
            if (kind < 0 || kind > static_cast<int>(domain::VersionKind::unknown)) throw std::invalid_argument("version");
            track.version.kind = static_cast<domain::VersionKind>(kind);
            if (!t.at("version_label").is_null()) track.version.label = t.at("version_label").get<std::string>();
            if (!strict_track_match(track, track)) throw std::invalid_argument("identity");
            state.original = std::move(track);
        }
        if (s != json::parse(encode_source_retry(state))) throw std::invalid_argument("fields");
        return state;
    } catch (const std::exception&) {
        return Error{ErrorCode::internal, "stored source retry state is invalid", false, {}};
    }
}

SourceRetryService::SourceRetryService(std::shared_ptr<const ports::ProviderDirectory> providers,
    std::shared_ptr<accounts::CredentialService> credentials, std::shared_ptr<ports::SourceStatisticsRepository> statistics)
    : providers_(std::move(providers)), credentials_(std::move(credentials)), statistics_(std::move(statistics)) {}

domain::Result<std::vector<ports::SourceDownloadStatistics>> SourceRetryService::statistics(const std::string& owner) {
    if (statistics_) return statistics_->source_statistics(owner);
    return std::vector<ports::SourceDownloadStatistics>{};
}

domain::Result<std::vector<domain::AccountBinding>> SourceRetryService::bindings(
    const std::string& owner, const std::optional<domain::AccountBinding>& original) {
    auto listed = providers_->list({owner, std::string{"resolve_audio"}, true, static_cast<std::uint32_t>(max_sources), {}});
    if (!listed) return listed.error();
    if (listed->next_cursor) return Error{ErrorCode::service_busy, "too many sources for bounded source retry", false, {}};
    std::vector<domain::AccountBinding> result;
    if (original) result.push_back(*original);
    for (const auto& view : listed->items) {
        if ((original && view.source == original->source()) || !view.supports("search_tracks") || !view.supports("track_detail")) continue;
        auto binding = credentials_->resolve_binding(owner, view.source.value(), "resolve_audio", {});
        if (binding) result.push_back(binding->binding);
        else if (binding.error().code == ErrorCode::storage_unavailable || binding.error().code == ErrorCode::internal)
            return binding.error();
    }
    if (result.size() > max_sources) return Error::validation("sources", "too many sources");
    return result;
}

domain::Result<ports::ProviderContext> SourceRetryService::context(const std::string& owner, const std::string& source,
    const std::string& capability, ports::OperationContext operation) {
    if (auto error = stopped(operation)) return *error;
    auto binding = credentials_->resolve_binding(owner, source, capability, {});
    if (!binding) return binding.error();
    auto lease = credentials_->lease(binding->binding);
    if (!lease) return lease.error();
    std::shared_ptr<const ports::SecretBytes> secret;
    if (lease->secret) secret = std::make_shared<const ports::SecretBytes>(std::move(*lease->secret));
    return ports::ProviderContext{operation, lease->identity, {}, std::move(secret)};
}

domain::Result<domain::Track> SourceRetryService::identity(const std::string& owner, const domain::TrackRef& track,
                                                         ports::OperationContext operation) {
    auto provider = providers_->track_detail(track.source.value());
    if (!provider) return provider.error();
    auto call = context(owner, track.source.value(), "track_detail", operation);
    if (!call) return call.error();
    auto identity = (*provider)->track_detail(*call, track);
    if (auto error = stopped(operation)) return *error;
    if (!identity) return identity.error();
    if (identity->ref != track || !strict_track_match(*identity, *identity))
        return Error{ErrorCode::source_exhausted, "original track identity is incomplete; strict source retry is unavailable", false, {}};
    return identity;
}

domain::Result<domain::Track> SourceRetryService::next(const std::string& owner, const domain::Track& original,
    const std::vector<domain::AccountBinding>& bindings, std::vector<std::string>& attempted, ports::OperationContext operation) {
    if (!strict_track_match(original, original) || bindings.size() > max_sources || attempted.size() > max_sources)
        return exhausted(attempted);
    auto stats = statistics(owner);
    if (!stats) return stats.error();
    auto ordered = bindings;
    const auto score = [&](const domain::AccountBinding& binding) {
        for (const auto& stat : *stats) if (stat.source == binding.source().value()) return stat.score();
        return 0.5;
    };
    std::sort(ordered.begin(), ordered.end(), [&](const auto& a, const auto& b) {
        if (score(a) != score(b)) return score(a) > score(b);
        return a.source().value() < b.source().value();
    });
    for (const auto& binding : ordered) {
        if (auto error = stopped(operation)) return *error;
        const auto& source = binding.source().value();
        if (source == original.ref.source.value() || std::find(attempted.begin(), attempted.end(), source) != attempted.end()) continue;
        if (attempted.size() == max_sources) break;
        auto search = providers_->track_search(source);
        auto detail = providers_->track_detail(source);
        if (!search || !detail) continue;
        // 每个平台共享 5 秒子预算；整次换源沿用调用方总预算和取消信号。
        auto per_source = operation;
        per_source.deadline = std::min(operation.deadline, std::chrono::steady_clock::now() + std::chrono::seconds{5});
        auto call = context(owner, source, "search_tracks", per_source);
        if (!call) {
            if (call.error().code == ErrorCode::service_busy || call.error().code == ErrorCode::storage_unavailable) return call.error();
            attempted.push_back(source);
            continue;
        }
        auto found = (*search)->search_tracks(*call, {original.title + " " + original.artists.front().name, search_limit, {}});
        if (auto error = stopped(operation)) return *error;
        if (!found && found.error().code == ErrorCode::service_busy) return found.error();
        attempted.push_back(source);
        if (!found) continue;
        auto details = context(owner, source, "track_detail", per_source);
        if (!details) {
            if (details.error().code == ErrorCode::service_busy || details.error().code == ErrorCode::storage_unavailable) {
                attempted.pop_back();
                return details.error();
            }
            continue;
        }
        std::uint32_t inspected = 0, confirmed = 0;
        for (const auto& candidate : found->page.items) {
            if (++inspected > search_limit || confirmed >= 3 || stopped(per_source)) break;
            if (candidate.ref.source != binding.source() || !strict_track_match(original, candidate)) continue;
            ++confirmed;
            auto exact = (*detail)->track_detail(*details, candidate.ref);
            if (auto error = stopped(operation)) return *error;
            if (!exact && (exact.error().code == ErrorCode::service_busy || exact.error().code == ErrorCode::storage_unavailable)) {
                attempted.pop_back();
                return exact.error();
            }
            if (exact && exact->ref == candidate.ref && strict_track_match(original, *exact)) return exact;
        }
    }
    if (auto error = stopped(operation)) return *error;
    return exhausted(attempted);
}
} // namespace music_service::application::catalog
