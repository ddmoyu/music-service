#include "api/httplib/control_api.hpp"

#include "application/accounts/scopes.hpp"

#ifdef _MSC_VER
// httplib.h has unreachable code (C4702) that MSVC reports while generating code
// for this file; it is third-party code, not ours.
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
#include <httplib.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <ctime>
#include <exception>
#include "application/ports/runtime/scope_exit.hpp"
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace music_service::api {

namespace {

using json = nlohmann::json;
namespace ports = application::ports;
namespace accounts = application::accounts;
using domain::ErrorCode;
using SteadyClock = std::chrono::steady_clock;

// ---------------------------------------------------------------- formatting

std::string utc(std::chrono::system_clock::time_point time) {
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    auto seconds = static_cast<std::time_t>(millis / 1000);
    auto ms = static_cast<int>(millis % 1000);
    if (ms < 0) {
        ms += 1000;
        --seconds;
    }
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif
    char buffer[32];
    const auto written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &parts);
    std::string out(buffer, written);
    const char digits[] = {static_cast<char>('0' + ms / 100), static_cast<char>('0' + ms / 10 % 10),
                           static_cast<char>('0' + ms % 10)};
    out.push_back('.');
    out.append(digits, 3);
    out.push_back('Z');
    return out;
}

json optional_time(const std::optional<std::chrono::system_clock::time_point>& time) {
    return time ? json(utc(*time)) : json(nullptr);
}

json optional_string(const std::optional<std::string>& value) { return value ? json(*value) : json(nullptr); }

// ------------------------------------------------------------------- errors

struct ApiError {
    int status{500};
    std::string code;
    std::string message;
    bool retryable{false};
    std::vector<domain::ErrorDetail> details;
};

int status_for(ErrorCode code) {
    switch (code) {
    case ErrorCode::validation_failed:
    case ErrorCode::unknown_provider:
    case ErrorCode::cursor_mismatch:
    case ErrorCode::multiple_ranges_unsupported:
        return 400;
    case ErrorCode::not_found:
        return 404;
    case ErrorCode::conflict:
    case ErrorCode::version_conflict:
    case ErrorCode::idempotency_conflict:
    case ErrorCode::task_finalizing:
    case ErrorCode::task_terminal:
    case ErrorCode::active_duplicate:
    case ErrorCode::invalid_transition:
        return 409;
    case ErrorCode::cursor_expired:
    case ErrorCode::resource_expired:
    case ErrorCode::file_missing:
    case ErrorCode::media_ticket_expired:
        return 410;
    case ErrorCode::capability_unsupported:
    case ErrorCode::provider_disabled:
    case ErrorCode::context_required:
    case ErrorCode::quality_unavailable:
    case ErrorCode::preview_rejected:
    case ErrorCode::credentials_required:
    case ErrorCode::destination_unavailable:
    case ErrorCode::delivery_unavailable:
    case ErrorCode::metadata_failed:
        return 422;
    case ErrorCode::queue_limit_exceeded:
    case ErrorCode::caller_limit_exceeded:
        return 429;
    case ErrorCode::upstream_failed:
    case ErrorCode::upstream_rate_limited:
    case ErrorCode::audio_invalid: // the upstream delivered something that is not the audio
        return 502;
    case ErrorCode::service_busy:
    case ErrorCode::storage_unavailable:
    case ErrorCode::instance_locked:
        return 503;
    case ErrorCode::upstream_timeout:
    case ErrorCode::deadline_exceeded:
        return 504;
    case ErrorCode::feature_unavailable:
        return 501;
    case ErrorCode::canceled:
    case ErrorCode::destination_root_changed:
    case ErrorCode::destination_in_use:
    case ErrorCode::file_system:
    case ErrorCode::finalization_failed:
    case ErrorCode::internal:
        break;
    }
    return 500;
}

ApiError from_domain(const domain::Error& error) {
    ApiError api{status_for(error.code), std::string{domain::to_string(error.code)}, error.message,
                 error.retryable, error.details};
    if (api.status == 500) {
        // Only a sanitized message and the request id leave the service.
        api.code = "INTERNAL_ERROR";
        api.message = "internal error";
        api.details.clear();
    }
    return api;
}

ApiError api_error(int status, std::string code, std::string message, std::vector<domain::ErrorDetail> details = {}) {
    return ApiError{status, std::move(code), std::move(message), false, std::move(details)};
}

ApiError bad_request(std::string field, std::string reason) {
    return api_error(400, "VALIDATION_FAILED", "request is invalid", {{std::move(field), std::move(reason)}});
}

json meta(const std::string& request_id) { return json{{"request_id", request_id}, {"partial", false}}; }

void send_data(httplib::Response& res, int status, json data, const std::string& request_id) {
    res.status = status;
    res.set_content(json{{"data", std::move(data)}, {"meta", meta(request_id)}, {"error", nullptr}}.dump(),
                    "application/json; charset=utf-8");
}

json error_json(const ApiError& error) {
    json details = json::array();
    for (const auto& detail : error.details) {
        details.push_back({{"field", detail.field}, {"reason", detail.reason}});
    }
    return json{{"code", error.code},
                {"message", error.message},
                {"retryable", error.retryable},
                {"details", std::move(details)}};
}

// Full envelope for responses that carry extra meta (search source results).
void send_envelope(httplib::Response& res, int status, json data, json meta_object, json error) {
    res.status = status;
    res.set_content(json{{"data", std::move(data)}, {"meta", std::move(meta_object)}, {"error", std::move(error)}}.dump(),
                    "application/json; charset=utf-8");
}

void send_error(httplib::Response& res, const ApiError& error, const std::string& request_id) {
    json details = json::array();
    for (const auto& detail : error.details) {
        details.push_back({{"field", detail.field}, {"reason", detail.reason}});
    }
    res.status = error.status;
    res.set_content(json{{"data", nullptr},
                         {"meta", meta(request_id)},
                         {"error",
                          {{"code", error.code},
                           {"message", error.message},
                           {"retryable", error.retryable},
                           {"details", std::move(details)}}}}
                        .dump(),
                    "application/json; charset=utf-8");
}

// ---------------------------------------------------------------- helpers

bool valid_client_request_id(const std::string& id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
    });
}

bool is_json_content_type(const std::string& value) {
    const auto type = value.substr(0, value.find(';'));
    std::string lower;
    for (const char c : type) {
        if (c != ' ' && c != '\t') {
            lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
        }
    }
    return lower == "application/json";
}

std::string to_string(ports::CredentialStatus status) {
    switch (status) {
    case ports::CredentialStatus::invalid:
        return "invalid";
    case ports::CredentialStatus::revoked:
        return "revoked";
    case ports::CredentialStatus::active:
        break;
    }
    return "active";
}

json outcome_json(const std::optional<ports::VerifyOutcome>& outcome) {
    if (!outcome) {
        return nullptr;
    }
    switch (*outcome) {
    case ports::VerifyOutcome::valid:
        return "valid";
    case ports::VerifyOutcome::invalid:
        return "invalid";
    case ports::VerifyOutcome::unknown:
        break;
    }
    return "unknown";
}

// CredentialDescriptor: never the secret or its reference.
json credential_json(const ports::CredentialRecord& c) {
    return json{{"credential_id", c.id},
                {"source", c.source},
                {"label", c.label},
                {"kind", c.kind},
                {"auth_profile", c.auth_profile},
                {"scope_summary", c.scopes.empty() ? json(nullptr) : json(c.scopes)},
                {"expires_at", optional_time(c.expires_at)},
                {"version", c.version},
                {"status", to_string(c.status)},
                {"last_verified_at", optional_time(c.last_verified_at)},
                {"last_verify_result", outcome_json(c.last_verify_result)},
                {"created_at", utc(c.created_at)}};
}

json selection_json(const ports::DefaultSelection& selection) {
    return json{{"source", selection.source},
                {"credential_id", optional_string(selection.credential_id)},
                {"selection_version", selection.selection_version}};
}

json ref_json(const domain::PlatformRef& ref) { return json{{"source", ref.source.value()}, {"id", ref.id.value()}}; }

json optional_ref(const std::optional<domain::PlatformRef>& ref) { return ref ? ref_json(*ref) : json(nullptr); }

std::string_view version_kind(domain::VersionKind kind) {
    switch (kind) {
    case domain::VersionKind::studio:
        return "studio";
    case domain::VersionKind::live:
        return "live";
    case domain::VersionKind::remix:
        return "remix";
    case domain::VersionKind::cover:
        return "cover";
    case domain::VersionKind::instrumental:
        return "instrumental";
    case domain::VersionKind::unknown:
        break;
    }
    return "unknown";
}

json track_json(const domain::Track& track) {
    json artists = json::array();
    for (const auto& artist : track.artists) {
        artists.push_back({{"name", artist.name}, {"ref", optional_ref(artist.ref)}});
    }
    json evidence = json::array();
    for (const auto& item : track.version.evidence) {
        evidence.push_back(item);
    }
    return json{{"ref", ref_json(track.ref)},
                {"title", track.title},
                {"artists", std::move(artists)},
                {"album", track.album ? json{{"title", track.album->title}, {"ref", optional_ref(track.album->ref)}}
                                      : json(nullptr)},
                {"duration_ms", track.duration_ms ? json(*track.duration_ms) : json(nullptr)},
                {"version",
                 {{"kind", version_kind(track.version.kind)},
                  {"label", optional_string(track.version.label)},
                  {"evidence", std::move(evidence)}}},
                {"artwork", optional_string(track.artwork)},
                {"track_number", track.track_number ? json(*track.track_number) : json(nullptr)},
                {"track_total", track.track_total ? json(*track.track_total) : json(nullptr)},
                // No supported source needs private context yet.
                {"context_ref", nullptr}};
}

json optional_uint(const std::optional<std::uint64_t>& value) { return value ? json(*value) : json(nullptr); }

json artists_json(const std::vector<domain::ArtistSummary>& artists) {
    json out = json::array();
    for (const auto& artist : artists) {
        out.push_back({{"name", artist.name}, {"ref", optional_ref(artist.ref)}});
    }
    return out;
}

std::string_view listed_name(domain::ListedKind kind) {
    switch (kind) {
    case domain::ListedKind::playlist:
        return "playlist";
    case domain::ListedKind::track:
        return "track";
    case domain::ListedKind::album:
        return "album";
    case domain::ListedKind::artist:
        return "artist";
    }
    return "playlist";
}

json playlist_json(const domain::Playlist& p) {
    return json{{"ref", ref_json(p.ref)},
                {"name", p.name},
                {"creator", optional_string(p.creator)},
                {"description", optional_string(p.description)},
                {"artwork", optional_string(p.artwork)},
                {"track_count", optional_uint(p.track_count)},
                {"play_count", optional_uint(p.play_count)},
                {"updated_at", optional_string(p.updated_at)}, {"relationship", optional_string(p.relationship)}};
}

json album_json(const domain::Album& a) {
    return json{{"ref", ref_json(a.ref)},
                {"title", a.title},
                {"artists", artists_json(a.artists)},
                {"release_date", optional_string(a.release_date)},
                {"track_count", optional_uint(a.track_count)},
                {"artwork", optional_string(a.artwork)},
                {"description", optional_string(a.description)}};
}

json artist_json(const domain::Artist& a) {
    return json{{"ref", ref_json(a.ref)}, {"name", a.name}, {"artwork", optional_string(a.artwork)},
        {"description", optional_string(a.description)}, {"alternate_name", optional_string(a.alternate_name)},
        {"country", optional_string(a.country)}, {"birth_date", optional_string(a.birth_date)},
        {"birthplace", optional_string(a.birthplace)}, {"gender", optional_string(a.gender)},
        {"language", optional_string(a.language)}, {"height", optional_string(a.height)},
        {"weight", optional_string(a.weight)}, {"constellation", optional_string(a.constellation)},
        {"track_count", optional_uint(a.track_count)}, {"album_count", optional_uint(a.album_count)},
        {"mv_count", optional_uint(a.mv_count)}};
}

json chart_json(const domain::Chart& c) {
    return json{{"ref", ref_json(c.ref)},
                {"name", c.name},
                {"description", optional_string(c.description)},
                {"update_hint", optional_string(c.update_hint)},
                {"updated_at", optional_string(c.updated_at)},
                {"track_count", optional_uint(c.track_count)},
                {"lists", listed_name(c.lists)}};
}

json lyrics_json(const domain::TrackLyrics& l) {
    json karaoke = json::array();
    for (const auto& line : l.karaoke) {
        json words = json::array();
        for (const auto& word : line.words) {
            words.push_back({{"start_ms", word.start_ms}, {"duration_ms", word.duration_ms}, {"text", word.text}});
        }
        karaoke.push_back({{"start_ms", line.start_ms}, {"duration_ms", line.duration_ms}, {"words", std::move(words)}});
    }
    return json{{"lrc", optional_string(l.lrc)},
                {"translated_lrc", optional_string(l.translated_lrc)},
                {"romanized_lrc", optional_string(l.romanized_lrc)},
                {"karaoke", std::move(karaoke)},
                {"unavailable", l.unavailable},
                {"warnings", l.warnings}};
}

json podcast_json(const domain::Podcast& p) {
    return {{"ref", ref_json(p.ref)}, {"title", p.title}, {"presenter", optional_string(p.presenter)},
        {"description", optional_string(p.description)}, {"artwork", optional_string(p.artwork)},
        {"episode_count", optional_uint(p.episode_count)}};
}
json episode_json(const domain::Episode& e) {
    return {{"ref", ref_json(e.ref)}, {"podcast", ref_json(e.podcast)}, {"title", e.title},
        {"podcast_title", e.podcast_title}, {"presenter", optional_string(e.presenter)},
        {"description", optional_string(e.description)}, {"artwork", optional_string(e.artwork)},
        {"duration_ms", e.duration_ms ? json(*e.duration_ms) : json(nullptr)},
        {"published_at", optional_string(e.published_at)}, {"audio_ref", e.audio_ref ? ref_json(*e.audio_ref) : json(nullptr)},
        {"requires_purchase", e.requires_purchase}};
}

json page_json(const application::catalog::SourcePage& p) {
    return json{{"limit", p.limit},
                {"returned", p.returned},
                {"total", optional_uint(p.total)},
                {"has_more", p.has_more ? json(*p.has_more) : json(nullptr)},
                {"next_cursor", optional_string(p.next_cursor)},
                {"listed_total", optional_uint(p.listed_total)},
                {"unavailable_count", optional_uint(p.unavailable_count)}};
}

json catalog_kind_json(const std::optional<ports::CatalogKind>& kind) {
    if (!kind) {
        return nullptr;
    }
    switch (*kind) {
    case ports::CatalogKind::live:
        return "live";
    case ports::CatalogKind::built_in:
        return "built_in";
    case ports::CatalogKind::search_derived:
        return "search_derived";
    }
    return nullptr;
}

json warnings_json(const std::vector<domain::ResourceWarning>& warnings) {
    json out = json::array();
    for (const auto warning : warnings) {
        std::string_view message = "the resource could not be fully verified";
        switch (warning) {
        case domain::ResourceWarning::full_length_unverified:
            message = "no sufficient evidence of the full length";
            break;
        case domain::ResourceWarning::quality_unverified:
            message = "the actual quality is not verified";
            break;
        case domain::ResourceWarning::quality_lowered:
            message = "a lower quality than requested was accepted";
            break;
        case domain::ResourceWarning::preview_resource:
            message = "this is a preview, not the full track";
            break;
        }
        out.push_back({{"code", domain::to_string(warning)}, {"message", message}});
    }
    return out;
}

json optional_int(const std::optional<std::int64_t>& value) { return value ? json(*value) : json(nullptr); }

// Request bodies are hashed as canonical JSON (sorted keys, no spaces), so reordered
// fields are the same request; different values, even equal to defaults, are not.
std::string body_hash(const json& body) {
    const auto text = body.dump();
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return "fnv1a64:" + std::to_string(text.size()) + ":" + std::to_string(hash);
}

json parsed_or_null(const std::optional<std::string>& text) {
    if (!text) {
        return nullptr;
    }
    auto parsed = json::parse(*text, nullptr, false);
    return parsed.is_discarded() ? json(nullptr) : parsed;
}

// The public facts a runner recorded: actual track, quality and warnings (codes).
json execution_field(const ports::TaskRecord& task, const char* key) {
    const auto execution = parsed_or_null(task.execution_json);
    return execution.is_object() && execution.contains(key) ? execution[key] : json(nullptr);
}

json task_warnings(const ports::TaskRecord& task) {
    json out = json::array();
    const auto codes = execution_field(task, "warnings");
    if (codes.is_array()) {
        for (const auto& code : codes) {
            const auto text = code.is_string() ? code.get<std::string>() : std::string{};
            for (const auto warning :
                 {domain::ResourceWarning::full_length_unverified, domain::ResourceWarning::quality_unverified,
                  domain::ResourceWarning::quality_lowered, domain::ResourceWarning::preview_resource}) {
                if (text == domain::to_string(warning)) {
                    out.push_back(warnings_json({warning})[0]);
                }
            }
        }
    }
    return out;
}

// The stored result, with the link to the published file (api-contract.md 10.1).
json result_json(const ports::TaskRecord& task) {
    auto result = parsed_or_null(task.result_json);
    if (result.is_object() && result.contains("file_id") && result["file_id"].is_string()) {
        result["links"] = {{"file", "/api/v1/files/" + result["file_id"].get<std::string>()}};
    }
    return result;
}

json task_json(const application::tasks::TaskView& view) {
    const auto& t = view.task;
    const auto& intent = view.intent;
    const auto& requirement = intent.requirement();
    return json{{"task_id", t.task_id},
                {"version", t.version},
                {"status", domain::to_string(t.status)},
                {"stage", nullptr},
                {"original_track", ref_json(intent.original_track())},
                {"actual_track", execution_field(t, "actual_track")},
                {"requested_quality", domain::to_string(requirement.requested)},
                {"quality_policy", domain::to_string(requirement.policy)},
                {"allow_preview", requirement.allow_preview},
                {"actual_quality", execution_field(t, "actual_quality")},
                {"fallback", {{"mode", "disabled"}, {"sources", json::array()}}},
                {"metadata_policy", domain::to_string(intent.metadata_policy())},
                {"attempt", t.attempt},
                {"destination_id", t.destination_id},
                {"progress", parsed_or_null(t.progress_json)},
                {"created_at", utc(t.created_at)},
                {"updated_at", utc(t.updated_at)},
                {"started_at", optional_time(t.started_at)},
                {"finished_at", optional_time(t.finished_at)},
                {"next_retry_at", optional_time(t.next_retry_at)},
                {"wait_reason", t.wait_reason ? json(domain::to_string(*t.wait_reason)) : json(nullptr)},
                {"retry_of", optional_string(t.retry_of)},
                {"result", result_json(t)},
                {"error", parsed_or_null(t.error_json)},
                {"warnings", task_warnings(t)},
                {"links", {{"self", "/api/v1/downloads/" + t.task_id}}}};
}

std::string task_etag(const ports::TaskRecord& task) {
    return "W/\"task-" + task.task_id + "-v" + std::to_string(task.version) + "\"";
}

// A single download from its JSON object; field names in errors carry prefix.
domain::Result<application::tasks::DownloadRequest> parse_download(const json& body, const std::string& prefix) {
    static const std::set<std::string> allowed{"track",           "context_ref",     "quality",       "quality_policy",
                                               "allow_preview",   "fallback",        "destination_id", "metadata_policy",
                                               "duplicate_policy", "credential_id",  "match_selection"};
    const auto bad = [&](const std::string& field, std::string reason) {
        return domain::Error::validation(prefix + field, std::move(reason));
    };
    if (!body.is_object()) {
        return domain::Error::validation(prefix.empty() ? "body" : prefix.substr(0, prefix.size() - 1),
                                         "must be an object");
    }
    for (const auto& [key, value] : body.items()) {
        if (allowed.count(key) == 0) {
            return bad(key, "unknown field");
        }
    }
    const auto track = body.find("track");
    if (track == body.end() || !track->is_object() || track->size() != 2 || !track->contains("source") ||
        !track->contains("id") || !(*track)["source"].is_string() || !(*track)["id"].is_string()) {
        return bad("track", "must be {source, id}");
    }
    auto source = domain::SourceId::parse((*track)["source"].get<std::string>());
    auto id = domain::PlatformId::parse((*track)["id"].get<std::string>());
    if (!source || !id) {
        return bad("track", "has an invalid source or id");
    }
    application::tasks::DownloadRequest request{{std::move(source).value(), std::move(id).value()}, {}, false, false,
                                                std::nullopt, std::nullopt, domain::MetadataPolicy::best_effort, false};
    const auto text = [&](const char* key, std::optional<std::string>& out) -> domain::Result<void> {
        const auto it = body.find(key);
        if (it == body.end() || it->is_null()) {
            return domain::ok();
        }
        if (!it->is_string()) {
            return bad(key, "must be a string");
        }
        out = it->get<std::string>();
        return domain::ok();
    };
    std::optional<std::string> quality, policy, metadata, duplicate;
    for (auto [key, out] : {std::pair{"quality", &quality}, std::pair{"quality_policy", &policy},
                            std::pair{"metadata_policy", &metadata}, std::pair{"duplicate_policy", &duplicate},
                            std::pair{"destination_id", &request.destination_id},
                            std::pair{"credential_id", &request.credential_id}}) {
        if (auto read = text(key, *out); !read) {
            return read.error();
        }
    }
    if (quality) {
        auto parsed = domain::parse_quality(*quality);
        if (!parsed) {
            return bad("quality", "must be standard, high, lossless or hires");
        }
        request.requirement.requested = parsed.value();
    }
    if (policy) {
        auto parsed = domain::parse_quality_policy(*policy);
        if (!parsed) {
            return bad("quality_policy", "must be strict or allow_lower");
        }
        request.requirement.policy = parsed.value();
    }
    if (metadata) {
        auto parsed = domain::parse_metadata_policy(*metadata);
        if (!parsed) {
            return bad("metadata_policy", "must be best_effort, required or skip");
        }
        request.metadata_policy = parsed.value();
    }
    if (duplicate && *duplicate != "reuse" && *duplicate != "force") {
        return bad("duplicate_policy", "must be reuse or force");
    }
    request.force = duplicate == "force";
    if (const auto preview = body.find("allow_preview"); preview != body.end()) {
        if (!preview->is_boolean()) {
            return bad("allow_preview", "must be a boolean");
        }
        request.requirement.allow_preview = preview->get<bool>();
    }
    if (const auto fallback = body.find("fallback"); fallback != body.end()) {
        if (!fallback->is_object() || !fallback->contains("mode") || !(*fallback)["mode"].is_string()) {
            return bad("fallback", "must be {mode, sources}");
        }
        for (const auto& [key, value] : fallback->items()) {
            if (key != "mode" && key != "sources") {
                return bad("fallback." + key, "unknown field");
            }
        }
        const auto mode = (*fallback)["mode"].get<std::string>();
        if (mode != "disabled" && mode != "auto") {
            return bad("fallback.mode", "must be disabled or auto");
        }
        request.automatic_fallback = mode == "auto";
    }
    request.match_selection = body.contains("match_selection") && !body["match_selection"].is_null();
    return request;
}

json string_array(const std::vector<std::string>& values) {
    json out = json::array();
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}

json capability_json(const ports::CapabilityView& capability) {
    const auto& spec = capability.spec;
    json out{{"supported", true},
             {"enabled", capability.enabled},
             {"requires_credentials", spec.credentials == ports::CredentialRequirement::required},
             {"credentials", ports::to_string(spec.credentials)},
             {"pagination", ports::to_string(spec.pagination)},
             {"requirements",
              {{"auth_profiles", string_array(spec.auth_profiles)},
               {"scopes", string_array(spec.scopes)},
               {"regions", string_array(spec.regions)},
               {"dependencies", string_array(spec.dependencies)}}},
             // No probing: availability is only known once calls report it.
             {"availability", {{"state", "unknown"}, {"reason", nullptr}, {"checked_at", nullptr}}}};
    if (spec.catalog_kind) {
        out["catalog_kind"] = ports::to_string(*spec.catalog_kind);
    }
    return out;
}

json provider_json(const ports::ProviderView& view, json credentials) {
    json capabilities = json::object();
    for (const auto& capability : view.capabilities) {
        capabilities[capability.spec.key] = capability_json(capability);
    }
    json methods = json::array();
    for (const auto& method : view.auth_methods) {
        json fields = json::array();
        for (const auto& field : method.input_fields) {
            fields.push_back({{"name", field.name}, {"required", field.required}, {"secret", field.secret}});
        }
        methods.push_back({{"auth_profile", method.auth_profile},
                           {"kind", ports::to_string(method.kind)},
                           {"version", method.version},
                           {"input_fields", std::move(fields)},
                           {"scopes", string_array(method.scopes)},
                           {"supports_verify", method.supports_verify},
                           {"supports_refresh", method.supports_refresh},
                           {"supports_application_token", method.supports_application_token}});
    }
    json qualities = json::array();
    for (const auto quality : view.quality_hints) {
        qualities.push_back(domain::to_string(quality));
    }
    return json{{"source", view.source.value()},
                {"name", view.name},
                {"enabled", view.enabled},
                {"implementation",
                 {{"id", view.implementation_id},
                  {"version", view.implementation_version},
                  {"execution_mode", ports::to_string(view.execution_mode)}}},
                {"capabilities", std::move(capabilities)},
                {"auth_methods", std::move(methods)},
                {"quality_hints", std::move(qualities)},
                {"credentials", std::move(credentials)}};
}

// One request on a route: identity, parsed input and the response.
struct Call {
    const httplib::Request& req;
    httplib::Response& res;
    std::string request_id;
    std::optional<ports::AuthenticatedToken> session;

    [[nodiscard]] const std::string& principal() const { return session->principal.id; }
    void ok(json data, int status = 200) const { send_data(res, status, std::move(data), request_id); }
    void fail(const ApiError& error) const { send_error(res, error, request_id); }
    void fail(const domain::Error& error) const { send_error(res, from_domain(error), request_id); }
};

// Unknown query parameters are rejected rather than silently ignored.
bool only_params(const Call& call, std::initializer_list<std::string_view> allowed) {
    for (const auto& [name, value] : call.req.params) {
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            call.fail(bad_request(name, "unknown parameter"));
            return false;
        }
    }
    return true;
}

// The JSON object body, with only the allowed keys. Sends 400 and returns nullopt
// otherwise. An empty body counts as {} when allow_empty.
std::optional<json> body_object(const Call& call, std::initializer_list<std::string_view> allowed,
                                bool allow_empty = false) {
    if (call.req.body.empty()) {
        if (allow_empty) {
            return json::object();
        }
        call.fail(bad_request("body", "a JSON object is required"));
        return std::nullopt;
    }
    auto parsed = json::parse(call.req.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        call.fail(bad_request("body", "must be a JSON object"));
        return std::nullopt;
    }
    for (const auto& [key, value] : parsed.items()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            call.fail(bad_request(key, "unknown field"));
            return std::nullopt;
        }
    }
    return parsed;
}

// Reads an optional string member; sends 400 for another type.
bool read_string(const Call& call, const json& object, const char* key, std::optional<std::string>& out) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return true;
    }
    if (!it->is_string()) {
        call.fail(bad_request(key, "must be a string"));
        return false;
    }
    out = it->get<std::string>();
    return true;
}

// The cookie from {"secret": {"cookie": "..."}} or the {"cookie": "..."} shorthand.
bool read_cookie(const Call& call, const json& body, std::optional<std::string>& cookie) {
    const auto secret = body.find("secret");
    const auto shorthand = body.find("cookie");
    if (secret != body.end() && shorthand != body.end()) {
        call.fail(bad_request("cookie", "cannot be combined with secret"));
        return false;
    }
    if (shorthand != body.end()) {
        return read_string(call, body, "cookie", cookie);
    }
    if (secret != body.end()) {
        if (!secret->is_object()) {
            call.fail(bad_request("secret", "must be an object"));
            return false;
        }
        for (const auto& [key, value] : secret->items()) {
            if (key != "cookie") {
                call.fail(bad_request("secret." + key, "unknown field"));
                return false;
            }
        }
        return read_string(call, *secret, "cookie", cookie);
    }
    return true;
}

// Cookie 保留原始头格式；其他 profile 的结构化输入交由注册的平台校验。
bool read_credential_secret(const Call& call, const json& body, std::string_view kind,
                            std::optional<std::string>& value) {
    if (kind == "cookie") return read_cookie(call, body, value);
    if (body.contains("cookie")) {
        call.fail(bad_request("cookie", "is only available for cookie credentials"));
        return false;
    }
    if (auto it = body.find("secret"); it != body.end()) {
        if (!it->is_object()) {
            call.fail(bad_request("secret", "must be an object"));
            return false;
        }
        value = it->dump();
    }
    return true;
}

thread_local SteadyClock::time_point request_started;

} // namespace

struct ControlApi::Impl {
    ControlApiOptions options;
    ControlApiServices services;
    httplib::Server server;
    std::thread thread;
    std::atomic<std::uint16_t> port{0};
    std::once_flag stopped;
    std::mutex admission_mutex;
    std::uint32_t admitted{0};
    std::map<std::string, std::uint32_t> admitted_by_owner;

    std::uint32_t admission_limit() const {
        // Keep at least one listener worker available for local control operations.
        return std::min(options.admission.global, std::max(options.listener.workers, 2u) - 1);
    }

    bool admit(Call& call) {
        const std::lock_guard lock{admission_mutex};
        const auto owner = admitted_by_owner.find(call.principal());
        if (owner != admitted_by_owner.end() && owner->second >= options.admission.per_principal) {
            call.res.set_header("Retry-After", "1");
            call.fail(api_error(429, "CALLER_LIMIT_EXCEEDED", "too many concurrent requests for this principal"));
            return false;
        }
        if (admitted >= admission_limit()) {
            call.res.set_header("Retry-After", "1");
            call.fail(api_error(503, "SERVICE_BUSY", "upstream request capacity is full"));
            return false;
        }
        ++admitted_by_owner[call.principal()];
        ++admitted;
        return true;
    }

    void release(const std::string& owner) noexcept {
        const std::lock_guard lock{admission_mutex};
        if (const auto it = admitted_by_owner.find(owner); it != admitted_by_owner.end()) {
            if (--it->second == 0) admitted_by_owner.erase(it);
            --admitted;
        }
    }

    Impl(ControlApiOptions o, ControlApiServices s) : options(std::move(o)), services(std::move(s)) {
        configure();
        routes();
    }

    [[nodiscard]] bool origin_allowed(const std::string& origin) const {
        const auto& allowed = options.cors.allowed_origins;
        return !origin.empty() && std::find(allowed.begin(), allowed.end(), origin) != allowed.end();
    }

    void configure() {
        const auto workers = std::max<std::uint32_t>(options.listener.workers, 2);
        const auto queue = options.listener.connection_queue;
        server.new_task_queue = [workers, queue] { return new httplib::ThreadPool(workers, workers, queue); };
        server.set_payload_max_length(options.http.body_limit_bytes);
        server.set_read_timeout(static_cast<time_t>(options.http.read_idle.count()));
        server.set_write_timeout(static_cast<time_t>(options.http.write_idle.count()));
        server.set_keep_alive_timeout(static_cast<time_t>(options.http.keep_alive.count()));
        server.set_keep_alive_max_count(options.http.max_requests_per_connection);

        server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            request_started = SteadyClock::now();
            auto id = req.get_header_value("X-Request-Id");
            if (!valid_client_request_id(id)) {
                id = services.new_request_id();
            }
            res.set_header("X-Request-Id", id);
            const auto origin = req.get_header_value("Origin");
            if (origin_allowed(origin)) {
                res.set_header("Access-Control-Allow-Origin", origin);
                res.set_header("Vary", "Origin");
                res.set_header("Access-Control-Expose-Headers", "ETag, Retry-After, X-Request-Id");
            }
            if (req.method == "OPTIONS") {
                // Preflight never reaches a route or a platform.
                if (!origin_allowed(origin)) {
                    send_error(res, api_error(403, "FORBIDDEN", "origin is not allowed"), id);
                    return httplib::Server::HandlerResponse::Handled;
                }
                res.status = 204;
                res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
                res.set_header("Access-Control-Allow-Headers",
                               "Authorization, Content-Type, Idempotency-Key, If-None-Match, X-Request-Id");
                res.set_header("Access-Control-Max-Age", "600");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        // Library-generated errors (unknown route, oversized body) get the envelope too.
        server.set_error_handler([this](const httplib::Request&, httplib::Response& res) {
            if (!res.body.empty()) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            auto id = res.get_header_value("X-Request-Id");
            if (id.empty()) {
                id = services.new_request_id();
                res.set_header("X-Request-Id", id);
            }
            const auto status = res.status;
            if (status == 404) {
                send_error(res, api_error(404, "NOT_FOUND", "no such route"), id);
            } else if (status == 405) {
                send_error(res, api_error(405, "METHOD_NOT_ALLOWED", "method not allowed"), id);
            } else if (status == 413) {
                send_error(res, api_error(413, "PAYLOAD_TOO_LARGE", "request body is too large"), id);
            } else if (status >= 500) {
                send_error(res, api_error(500, "INTERNAL_ERROR", "internal error"), id);
            } else {
                send_error(res, api_error(status, "VALIDATION_FAILED", "request is invalid"), id);
            }
            res.status = status;
            return httplib::Server::HandlerResponse::Handled;
        });

        server.set_exception_handler([this](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
            auto id = res.get_header_value("X-Request-Id");
            send_error(res, api_error(500, "INTERNAL_ERROR", "internal error"), id.empty() ? services.new_request_id() : id);
        });

        server.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
            if (services.on_request) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - request_started);
                try {
                    services.on_request(res.get_header_value("X-Request-Id"), req.method, req.path, res.status, elapsed);
                } catch (...) {} // diagnostics cannot escape a library worker thread
            }
        });
    }

    enum class Access { open, any_scope, scoped };

    // Wraps a route with authentication, scope and content-type checks.
    httplib::Server::Handler guard(Access access, std::string_view scope, std::function<void(Call&)> route,
                                  bool upstream = false) {
        return [this, access, scope = std::string{scope}, route = std::move(route), upstream](const httplib::Request& req,
                                                                                   httplib::Response& res) {
            Call call{req, res, res.get_header_value("X-Request-Id"), std::nullopt};
            if (access != Access::open) {
                res.set_header("Cache-Control", "no-store");
                const auto header = req.get_header_value("Authorization");
                constexpr std::string_view bearer = "Bearer ";
                std::optional<ports::AuthenticatedToken> session;
                if (header.size() > bearer.size() && std::string_view{header}.substr(0, bearer.size()) == bearer) {
                    auto found = services.authenticator->authenticate(std::string_view{header}.substr(bearer.size()));
                    if (!found) {
                        call.fail(found.error());
                        return;
                    }
                    session = std::move(found).value();
                }
                if (!session) {
                    res.set_header("WWW-Authenticate", "Bearer");
                    call.fail(api_error(401, "UNAUTHENTICATED", "a valid bearer token is required"));
                    return;
                }
                if (access == Access::scoped && !accounts::has_scope(session->token.scopes, scope)) {
                    call.fail(api_error(403, "FORBIDDEN", "the token lacks the " + scope + " scope"));
                    return;
                }
                call.session = std::move(session);
            }
            if (!req.body.empty() && !is_json_content_type(req.get_header_value("Content-Type"))) {
                call.fail(api_error(415, "UNSUPPORTED_CONTENT_TYPE", "request bodies must be application/json"));
                return;
            }
            if (upstream) {
                // cpp-httplib dedicates a worker to each keep-alive connection.
                // Closing slow/rejected connections also releases that reservation.
                res.set_header("Connection", "close");
                if (!admit(call)) return;
                const ports::ScopeExit release_slot{[this, &call]() noexcept { release(call.principal()); }};
                route(call);
            } else {
                route(call);
            }
        };
    }

    void routes() {
        server.Get("/health/live", guard(Access::open, {}, [](Call& call) { call.ok({{"status", "live"}}); }));
        server.Get("/health/ready", guard(Access::open, {}, [this](Call& call) {
            if (services.ready && services.ready()) {
                call.ok({{"status", "ready"}});
            } else {
                call.fail(api_error(503, "SERVICE_BUSY", "the service is not ready"));
            }
        }));
        server.Get("/api/v1/info", guard(Access::any_scope, {}, [this](Call& call) { info(call); }));
        server.Get("/api/v1/providers",
                   guard(Access::scoped, accounts::scope::catalog_read, [this](Call& call) { providers(call); }));
        server.Get("/api/v1/limits",
                   guard(Access::scoped, accounts::scope::catalog_read, [this](Call& call) { limits(call); }));
        server.Get("/api/v1/destinations",
                   guard(Access::scoped, accounts::scope::downloads_read, [this](Call& call) { destinations(call); }));

        const auto credentials = accounts::scope::credentials_write;
        server.Get("/api/v1/credentials", guard(Access::scoped, credentials, [this](Call& call) { list_credentials(call); }));
        server.Post("/api/v1/credentials", guard(Access::scoped, credentials, [this](Call& call) { create_credential(call); }));
        server.Put(R"(/api/v1/credentials/([A-Za-z0-9_]+))",
                   guard(Access::scoped, credentials, [this](Call& call) { update_credential(call); }));
        server.Post(R"(/api/v1/credentials/([A-Za-z0-9_]+)/verify)",
                    guard(Access::scoped, credentials, [this](Call& call) { verify_credential(call); }, true));
        server.Delete(R"(/api/v1/credentials/([A-Za-z0-9_]+))",
                      guard(Access::scoped, credentials, [this](Call& call) { revoke_credential(call); }));
        server.Get(R"(/api/v1/credential-defaults/([a-z0-9_]+))",
                   guard(Access::scoped, credentials, [this](Call& call) { get_default(call); }));
        server.Put(R"(/api/v1/credential-defaults/([a-z0-9_]+))",
                   guard(Access::scoped, credentials, [this](Call& call) { set_default(call); }));
        server.Delete(R"(/api/v1/credential-defaults/([a-z0-9_]+))",
                      guard(Access::scoped, credentials, [this](Call& call) { clear_default(call); }));

        const auto catalog = accounts::scope::catalog_read;
        server.Get("/api/v1/search/tracks", guard(Access::scoped, catalog, [this](Call& call) { search_tracks(call); }, true));
        server.Get("/api/v1/tracks/detail", guard(Access::scoped, catalog, [this](Call& call) { track_detail(call); }, true));
        catalog_routes(catalog);
        server.Post("/api/v1/links/parse", guard(Access::scoped, catalog, [this](Call& call) { parse_link(call); }, true));
        server.Post("/api/v1/assets/resolve", guard(Access::scoped, catalog, [this](Call& call) { resolve_asset(call); }, true));
        server.Post("/api/v1/assets/probe", guard(Access::scoped, catalog, [this](Call& call) { probe_asset(call); }, true));

        const auto read = accounts::scope::downloads_read;
        const auto write = accounts::scope::downloads_write;
        server.Post("/api/v1/downloads", guard(Access::scoped, write, [this](Call& call) { create_download(call); }));
        server.Get("/api/v1/downloads", guard(Access::scoped, read, [this](Call& call) { list_downloads(call); }));
        server.Get(R"(/api/v1/downloads/(tsk_[A-Za-z0-9]+))",
                   guard(Access::scoped, read, [this](Call& call) { get_download(call); }));
        server.Post(R"(/api/v1/downloads/(tsk_[A-Za-z0-9]+)/cancel)",
                    guard(Access::scoped, write, [this](Call& call) { cancel_download(call); }));
        server.Post(R"(/api/v1/downloads/(tsk_[A-Za-z0-9]+)/retry)",
                    guard(Access::scoped, write, [this](Call& call) { retry_download(call); }));
        server.Post("/api/v1/download-batches", guard(Access::scoped, write, [this](Call& call) { create_batch(call); }));
        server.Get(R"(/api/v1/download-batches/(bat_[A-Za-z0-9]+))",
                   guard(Access::scoped, read, [this](Call& call) { get_batch(call); }));
        server.Post(R"(/api/v1/download-batches/(bat_[A-Za-z0-9]+)/cancel)",
                    guard(Access::scoped, write, [this](Call& call) { cancel_batch(call); }));

        server.Get(R"(/api/v1/files/(fil_[A-Za-z0-9]+))",
                   guard(Access::scoped, accounts::scope::files_read, [this](Call& call) { get_file(call); }));
        // The scope depends on the target kind: checked in the route.
        server.Post("/api/v1/media-access", guard(Access::any_scope, "", [this](Call& call) { media_access(call); }, true));

        server.Get("/api/v1/admin/status",
                   guard(Access::scoped, accounts::scope::admin, [this](Call& call) { admin_status(call); }));
        if (services.download_policy) {
            server.Get("/api/v1/admin/download-policy",
                       guard(Access::scoped, accounts::scope::admin, [this](Call& call) { get_download_policy(call); }));
            server.Patch("/api/v1/admin/download-policy", guard(Access::scoped, accounts::scope::admin,
                                                                [this](Call& call) { set_download_policy(call); }));
        }
    }

    // ------------------------------------------------------------ system

    void info(Call& call) {
        const auto& i = options.info;
        const auto& f = i.features;
        call.ok({{"service", "music-service"},
                 {"version", i.version},
                 {"api_version", "v1"},
                 {"instance_id", i.instance_id},
                 {"media_base_url", optional_string(i.media_base_url)},
                 {"events_base_url", optional_string(i.events_base_url)},
                 {"features",
                  {{"search", f.search},
                   {"downloads", f.downloads},
                   {"file_access", f.file_access},
                   {"media_proxy", f.media_proxy},
                   {"direct_media_urls", f.direct_media_urls},
                   {"automatic_fallback", f.automatic_fallback},
                   {"progress_polling", f.progress_polling},
                   {"progress_sse", f.progress_sse}}}});
    }

    void providers(Call& call) {
        if (!only_params(call, {"capability", "enabled", "limit", "cursor"})) {
            return;
        }
        ports::ProviderListQuery query;
        query.principal_id = call.principal();
        if (call.req.has_param("capability")) {
            query.capability = call.req.get_param_value("capability");
        }
        if (call.req.has_param("enabled")) {
            const auto value = call.req.get_param_value("enabled");
            if (value != "true" && value != "false") {
                call.fail(bad_request("enabled", "must be true or false"));
                return;
            }
            query.enabled = value == "true";
        }
        if (call.req.has_param("limit")) {
            const auto value = call.req.get_param_value("limit");
            unsigned limit = 0;
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), limit);
            if (ec != std::errc{} || end != value.data() + value.size() || limit < 1 || limit > 100) {
                call.fail(bad_request("limit", "must be between 1 and 100"));
                return;
            }
            query.limit = limit;
        }
        if (call.req.has_param("cursor")) {
            query.cursor = call.req.get_param_value("cursor");
        }
        auto page = services.providers->list(query);
        if (!page) {
            call.fail(page.error());
            return;
        }
        json items = json::array();
        for (const auto& view : page->items) {
            // The caller's own credential summary; never another principal's.
            const auto& source = view.source.value();
            auto owned = services.credentials->list(call.principal(), source);
            auto selection = services.credentials->get_default(call.principal(), source);
            if (!owned || !selection) {
                call.fail(!owned ? owned.error() : selection.error());
                return;
            }
            const auto active = std::count_if(owned->begin(), owned->end(), [](const ports::CredentialRecord& c) {
                return c.status != ports::CredentialStatus::revoked;
            });
            items.push_back(provider_json(view, {{"usable", active},
                                                 {"default_credential_id", optional_string(selection->credential_id)},
                                                 {"selection_version", selection->selection_version}}));
        }
        call.ok({{"items", std::move(items)},
                 {"page", {{"limit", query.limit}, {"next_cursor", optional_string(page->next_cursor)}}}});
    }

    void limits(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        const auto& c = options.catalog;
        call.ok({{"catalog",
                  {{"default_sources", string_array(c.default_sources)},
                   {"max_sources_per_request", c.max_sources_per_request},
                   {"per_source_limit", {{"default", 20}, {"max", 50}}},
                   {"timeout_ms", {{"default", c.total_budget.count()}, {"min", 1000}, {"max", c.max_budget.count()}}}}},
                 {"downloads",
                  {{"max_batch_items", options.download_limits.max_batch_items},
                   {"per_principal_active", options.download_limits.per_principal_active}}},
                 {"media", {{"per_principal", options.media_limits.per_principal}}},
                 {"admission", {{"global", admission_limit()}, {"per_principal", options.admission.per_principal}}}});
    }

    void destinations(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        json items = json::array();
        for (const auto& d : services.destinations(call.principal())) {
            items.push_back({{"destination_id", d.id},
                             {"name", d.name},
                             {"available", d.available},
                             {"accept_new_tasks", d.accept_new_tasks}});
        }
        call.ok({{"items", std::move(items)}});
    }

    void admin_status(Call& call) {
        const auto s = services.status();
        call.ok({{"state", s.state},
                 {"instance_id", s.instance_id},
                 {"version", s.version},
                 {"started_at", utc(s.started_at)},
                 {"storage", {{"available", s.storage_available}}},
                 {"providers", {{"registered", s.providers_registered}}},
                 {"destinations", {{"configured", s.destinations}}},
                 {"control", {{"workers", s.control_workers}}},
                 {"upstream", {{"connections_in_use", s.upstream_connections_in_use}, {"cooldowns", s.upstream_cooldowns}}},
                 {"downloads",
                  {{"workers", s.download_workers},
                   {"running", s.downloads_running},
                   {"paused", s.downloads_paused},
                   {"claimed", s.downloads_claimed}, {"scheduler_errors", s.scheduler_errors}}},
                 {"logging", {{"dropped", s.log_dropped}, {"failures", s.log_failures}, {"queued_bytes", s.log_queued_bytes}}},
                 {"media", {{"listening", s.media_listening}, {"responses", s.media_responses}}}});
    }

    // ------------------------------------------------------------- files

    [[nodiscard]] bool file_access_enabled() const { return services.files && options.info.features.file_access; }

    static json properties_of(const std::string& properties) {
        auto parsed = json::parse(properties, nullptr, false);
        return parsed.is_object() ? parsed : json::object();
    }

    void get_file(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        if (!file_access_enabled()) {
            call.fail(api_error(501, "FEATURE_UNAVAILABLE", "file access is not enabled"));
            return;
        }
        const std::string file_id = call.req.matches[1];
        auto asset = services.files->describe(call.principal(), file_id);
        if (!asset) {
            call.fail(asset.error());
            return;
        }
        const auto etag = "W/\"file-" + asset->id + "-v" + std::to_string(asset->version) + "\"";
        call.res.set_header("ETag", etag);
        if (call.req.get_header_value("If-None-Match") == etag) {
            call.res.status = 304;
            return;
        }
        const auto properties = properties_of(asset->properties_json);
        const auto field = [&](const char* key) { return properties.contains(key) ? properties[key] : json(nullptr); };
        call.ok({{"file_id", asset->id},
                 {"display_name", application::media::display_name(*asset)},
                 {"size_bytes", asset->size_bytes},
                 {"container", optional_string(asset->container)},
                 {"content_type", application::media::content_type_of(asset->container.value_or(""))},
                 {"actual_track", field("actual_track")},
                 {"quality", field("actual_quality")},
                 {"metadata_status", field("metadata_status")},
                 {"artifacts", field("bundle")},
                 {"etag", asset->etag},
                 {"status", "available"},
                 {"created_at", utc(asset->created_at)},
                 {"media_url", options.info.media_base_url ? json(*options.info.media_base_url + "/media/v1/files/" +
                                                                  asset->id)
                                                           : json(nullptr)},
                 {"links", {{"self", "/api/v1/files/" + asset->id}}}});
    }

    void media_access(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"target", "mode", "auth", "ttl_seconds"});
        if (!body) {
            return;
        }
        const auto target = body->find("target");
        if (target == body->end() || !target->is_object() || target->size() != 2 || !target->contains("kind") ||
            !target->contains("id") || !(*target)["kind"].is_string() || !(*target)["id"].is_string()) {
            call.fail(bad_request("target", "must be {kind, id}"));
            return;
        }
        application::media::AccessRequest request;
        request.owner_id = call.principal();
        request.token_id = call.session->token.id;
        const auto kind = (*target)["kind"].get<std::string>();
        if (kind == "file") {
            request.kind = application::media::TargetKind::file;
        } else if (kind == "asset") {
            request.kind = application::media::TargetKind::asset;
        } else {
            call.fail(bad_request("target.kind", "must be file or asset"));
            return;
        }
        request.target_id = (*target)["id"].get<std::string>();
        std::optional<std::string> mode;
        std::optional<std::string> auth;
        if (!read_string(call, *body, "mode", mode) || !read_string(call, *body, "auth", auth)) {
            return;
        }
        if (!mode || *mode == "auto") {
            request.mode = application::media::DeliveryMode::automatic;
        } else if (*mode == "direct") {
            request.mode = application::media::DeliveryMode::direct;
        } else if (*mode == "proxy") {
            request.mode = application::media::DeliveryMode::proxy;
        } else {
            call.fail(bad_request("mode", "must be auto, direct or proxy"));
            return;
        }
        if (!auth || *auth == "ticket") {
            request.auth = application::media::MediaAuth::ticket;
        } else if (*auth == "bearer") {
            request.auth = application::media::MediaAuth::bearer;
        } else {
            call.fail(bad_request("auth", "must be ticket or bearer"));
            return;
        }
        if (const auto ttl = body->find("ttl_seconds"); ttl != body->end()) {
            if (!ttl->is_number_integer() || ttl->get<std::int64_t>() < 15 || ttl->get<std::int64_t>() > 300) {
                call.fail(bad_request("ttl_seconds", "must be an integer from 15 to 300"));
                return;
            }
            request.ttl = std::chrono::seconds{ttl->get<std::int64_t>()};
        }
        const auto scope = request.kind == application::media::TargetKind::file ? accounts::scope::files_read
                                                                                : accounts::scope::media_read;
        if (!accounts::has_scope(call.session->token.scopes, scope)) {
            call.fail(api_error(403, "FORBIDDEN", "the token lacks the " + std::string{scope} + " scope"));
            return;
        }
        if (!services.media) {
            call.fail(api_error(501, "FEATURE_UNAVAILABLE", "media access is not enabled"));
            return;
        }
        auto described = services.media->describe(request);
        if (!described) {
            call.fail(described.error());
            return;
        }
        const auto& d = described.value();
        const auto properties = properties_of(d.properties_json);
        const auto field = [&](const char* key) { return properties.contains(key) ? properties[key] : json(nullptr); };
        const auto actual_track = d.actual_track ? ref_json(*d.actual_track) : field("actual_track");
        const auto actual_quality = d.actual_quality ? json(std::string{domain::to_string(*d.actual_quality)})
                                                     : field("actual_quality");
        // Ticket and direct URLs are capabilities: never stored by a cache.
        call.res.set_header("Cache-Control", "no-store");
        call.ok({{"target", {{"kind", application::media::to_string(d.kind)}, {"id", d.target_id}}},
                 {"delivery", d.delivery},
                 {"url", d.url},
                 {"auth", d.auth},
                 {"expires_at", optional_time(d.expires_at)},
                 {"content_type", d.content_type},
                 {"range_supported", d.range_supported ? json(*d.range_supported) : json(nullptr)},
                 {"actual_track", actual_track},
                 {"actual_quality", actual_quality}});
    }

    void get_download_policy(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        call.ok({{"paused", services.download_policy->paused()}});
    }

    // paused only stops claiming; running downloads go on, canceling stays a task command.
    void set_download_policy(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"paused"});
        if (!body) {
            return;
        }
        const auto paused = body->find("paused");
        if (paused == body->end() || !paused->is_boolean()) {
            call.fail(bad_request("paused", "must be true or false"));
            return;
        }
        services.download_policy->set_paused(paused->get<bool>());
        call.ok({{"paused", services.download_policy->paused()}});
    }

    // ----------------------------------------------------------- catalog

    static std::string_view status_name(application::catalog::SourceStatus status) {
        switch (status) {
        case application::catalog::SourceStatus::ok:
            return "ok";
        case application::catalog::SourceStatus::unsupported:
            return "unsupported";
        case application::catalog::SourceStatus::disabled:
            return "disabled";
        case application::catalog::SourceStatus::failed:
            break;
        }
        return "failed";
    }

    // T21: collections, catalogs and lyrics, all under catalog:read.
    void catalog_routes(std::string_view scope) {
        namespace cat = application::catalog;
        const auto route = [&](const char* path, std::function<void(Call&)> handler) {
            server.Get(path, guard(Access::scoped, scope, [handler = std::move(handler)](Call& call) { handler(call); }, true));
        };
        const auto c = [this] { return services.catalog; };
        route("/api/v1/search/playlists", [this, c](Call& call) {
            search_route(call, [c](const cat::SearchRequest& r) { return c()->search_playlists(r); }, playlist_json);
        });
        route("/api/v1/search/albums", [this, c](Call& call) {
            search_route(call, [c](const cat::SearchRequest& r) { return c()->search_albums(r); }, album_json);
        });
        route("/api/v1/search/artists", [this, c](Call& call) {
            search_route(call, [c](const cat::SearchRequest& r) { return c()->search_artists(r); }, artist_json);
        });
        const auto detail = [c](auto method) {
            return [c, method](const std::string& owner, const std::string& source, const std::string& id) {
                return ((*c()).*method)(owner, source, id);
            };
        };
        route("/api/v1/playlists/detail",
              [this, detail](Call& call) { detail_route(call, detail(&cat::CatalogService::playlist_detail), playlist_json); });
        route("/api/v1/albums/detail",
              [this, detail](Call& call) { detail_route(call, detail(&cat::CatalogService::album_detail), album_json); });
        route("/api/v1/artists/detail",
              [this, detail](Call& call) { detail_route(call, detail(&cat::CatalogService::artist_detail), artist_json); });
        route("/api/v1/charts/detail",
              [this, detail](Call& call) { detail_route(call, detail(&cat::CatalogService::chart_detail), chart_json); });
        route("/api/v1/lyrics",
              [this, detail](Call& call) { detail_route(call, detail(&cat::CatalogService::lyrics), lyrics_json); });
        route("/api/v1/podcasts/detail",
              [this, detail](Call& call) { detail_route(call, detail(&cat::CatalogService::podcast_detail), podcast_json); });
        route("/api/v1/episodes/detail",
              [this, detail](Call& call) { detail_route(call, detail(&cat::CatalogService::episode_detail), episode_json); });
        const auto list = [c](auto method) {
            return [c, method](const cat::ListRequest& request) { return ((*c()).*method)(request); };
        };
        route("/api/v1/playlists/tracks",
              [this, list](Call& call) { list_route(call, "id", list(&cat::CatalogService::playlist_tracks), track_json); });
        route("/api/v1/me/playlists",
              [this, list](Call& call) { list_route(call, nullptr, list(&cat::CatalogService::user_playlists), playlist_json); });
        route("/api/v1/podcasts/episodes",
              [this, list](Call& call) { list_route(call, "id", list(&cat::CatalogService::podcast_episodes), episode_json); });
        route("/api/v1/albums/tracks",
              [this, list](Call& call) { list_route(call, "id", list(&cat::CatalogService::album_tracks), track_json); });
        route("/api/v1/artists/tracks",
              [this, list](Call& call) { list_route(call, "id", list(&cat::CatalogService::artist_tracks), track_json); });
        route("/api/v1/artists/albums",
              [this, list](Call& call) { list_route(call, "id", list(&cat::CatalogService::artist_albums), album_json); });
        route("/api/v1/charts/tracks",
              [this, list](Call& call) { list_route(call, "id", list(&cat::CatalogService::chart_tracks), track_json); });
        route("/api/v1/categories/playlists", [this, list](Call& call) {
            list_route(call, "category_id", list(&cat::CatalogService::category_playlists), playlist_json);
        });
        route("/api/v1/categories/tracks", [this, list](Call& call) {
            list_route(call, "category_id", list(&cat::CatalogService::category_tracks), track_json);
        });
        route("/api/v1/recommendations/playlists", [this, list](Call& call) {
            list_route(call, nullptr, list(&cat::CatalogService::recommend_playlists), playlist_json);
        });
        route("/api/v1/categories", [this, c](Call& call) {
            directory_route(
                call, [c](const std::string& owner, const std::string& source) { return c()->categories(owner, source); },
                [](const domain::Category& category, const std::string& source) {
                    return json{{"source", source},
                                {"id", category.id},
                                {"name", category.name},
                                {"group", optional_string(category.group)},
                                {"lists", listed_name(category.lists)}};
                });
        });
        route("/api/v1/charts", [this, c](Call& call) {
            directory_route(
                call, [c](const std::string& owner, const std::string& source) { return c()->charts(owner, source); },
                [](const domain::Chart& chart, const std::string&) { return chart_json(chart); });
        });
    }

    void search_tracks(Call& call) {
        search_route(call, [this](const auto& r) { return services.catalog->search_tracks(r); }, track_json);
    }

    // GET /api/v1/search/{kind}: the same parameters, fan-out and per-source results for every kind.
    template <class Search, class ToJson> void search_route(Call& call, const Search& search, const ToJson& to_json) {
        if (!only_params(call, {"q", "sources", "limit", "cursor", "page", "timeout_ms"})) {
            return;
        }
        if (call.req.has_param("page")) {
            call.fail(bad_request("page", "page jumps are not supported by the available sources; use cursor"));
            return;
        }
        application::catalog::SearchRequest request;
        request.owner_id = call.principal();
        request.q = call.req.get_param_value("q");
        if (call.req.has_param("sources")) {
            const auto text = call.req.get_param_value("sources");
            if (text == "all") {
                request.all_sources = true;
            } else {
                std::size_t at = 0;
                while (at <= text.size()) {
                    const auto comma = text.find(',', at);
                    const auto part = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                    if (part.empty()) {
                        call.fail(bad_request("sources", "must be a comma-separated list of sources or all"));
                        return;
                    }
                    request.sources.push_back(part);
                    if (comma == std::string::npos) {
                        break;
                    }
                    at = comma + 1;
                }
            }
        }
        if (!read_uint_param(call, "limit", request.limit)) {
            return;
        }
        if (call.req.has_param("cursor")) {
            request.cursor = call.req.get_param_value("cursor");
        }
        if (call.req.has_param("timeout_ms")) {
            std::uint32_t timeout = 0;
            if (!read_uint_param(call, "timeout_ms", timeout)) {
                return;
            }
            request.timeout = std::chrono::milliseconds{timeout};
        }
        auto searched = search(request);
        if (!searched) {
            call.fail(searched.error());
            return;
        }
        auto& result = searched.value();
        json items = json::array();
        for (const auto& item : result.items) {
            items.push_back(to_json(item));
        }
        json sources = json::array();
        for (const auto& source : result.sources) {
            json page = nullptr;
            if (source.page) {
                page = page_json(*source.page);
            }
            json error = nullptr;
            if (source.error) {
                const auto api = from_domain(*source.error);
                error = {{"code", api.code}, {"message", api.message}, {"retryable", api.retryable}};
            }
            json cache = nullptr;
            if (source.cache) {
                cache = {{"hit", source.cache->hit}, {"created_at", utc(source.cache->created_at)}};
            }
            sources.push_back({{"source", source.source},
                               {"status", status_name(source.status)},
                               {"elapsed_ms", source.elapsed.count()},
                               {"page", std::move(page)},
                               {"error", std::move(error)},
                               {"cache", std::move(cache)}});
        }
        json meta_object{{"request_id", call.request_id}, {"partial", result.partial}, {"source_results", std::move(sources)}};
        if (result.failure) {
            const auto api = from_domain(*result.failure);
            send_envelope(call.res, api.status, nullptr, std::move(meta_object), error_json(api));
            return;
        }
        send_envelope(call.res, 200, {{"items", std::move(items)}}, std::move(meta_object), nullptr);
    }

    // Reads an optional unsigned integer parameter; sends 400 when malformed.
    static bool read_uint_param(const Call& call, const char* name, std::uint32_t& out) {
        if (!call.req.has_param(name)) {
            return true;
        }
        const auto value = call.req.get_param_value(name);
        std::uint32_t parsed = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || end != value.data() + value.size()) {
            call.fail(bad_request(name, "must be a non-negative integer"));
            return false;
        }
        out = parsed;
        return true;
    }

    // GET .../detail and /lyrics: source and id in the query.
    template <class Fetch, class ToJson> void detail_route(Call& call, const Fetch& fetch, const ToJson& to_json) {
        if (!only_params(call, {"source", "id", "context_ref"})) {
            return;
        }
        if (!call.req.has_param("source") || !call.req.has_param("id")) {
            call.fail(bad_request("source", "source and id are required"));
            return;
        }
        auto found = fetch(call.principal(), call.req.get_param_value("source"), call.req.get_param_value("id"));
        found ? call.ok(to_json(found->value)) : call.fail(found.error());
    }

    // GET .../tracks, /artists/albums, /categories/playlists, /recommendations/playlists:
    // one page of what an entity holds; id_param is the entity's parameter, if any.
    template <class Fetch, class ToJson>
    void list_route(Call& call, const char* id_param, const Fetch& fetch, const ToJson& to_json) {
        std::initializer_list<std::string_view> with_id{"source", id_param ? id_param : "id", "limit", "cursor"};
        std::initializer_list<std::string_view> without_id{"source", "limit", "cursor"};
        if (!only_params(call, id_param ? with_id : without_id)) {
            return;
        }
        if (!call.req.has_param("source") || (id_param && !call.req.has_param(id_param))) {
            call.fail(bad_request("source", id_param ? std::string{"source and "} + id_param + " are required"
                                                     : std::string{"source is required"}));
            return;
        }
        application::catalog::ListRequest request;
        request.owner_id = call.principal();
        request.source = call.req.get_param_value("source");
        if (id_param) {
            request.id = call.req.get_param_value(id_param);
        }
        if (call.req.has_param("limit")) {
            std::uint32_t limit = 0;
            if (!read_uint_param(call, "limit", limit)) {
                return;
            }
            request.limit = limit;
        }
        if (call.req.has_param("cursor")) {
            request.cursor = call.req.get_param_value("cursor");
        }
        auto listed = fetch(request);
        if (!listed) {
            call.fail(listed.error());
            return;
        }
        json items = json::array();
        for (const auto& item : listed->items) {
            items.push_back(to_json(item));
        }
        call.ok({{"items", std::move(items)}, {"page", page_json(listed->page)}});
    }

    // GET /api/v1/categories and /charts: a source's directory.
    template <class Fetch, class ToJson> void directory_route(Call& call, const Fetch& fetch, const ToJson& to_json) {
        if (!only_params(call, {"source"})) {
            return;
        }
        if (!call.req.has_param("source")) {
            call.fail(bad_request("source", "is required"));
            return;
        }
        const auto source = call.req.get_param_value("source");
        auto found = fetch(call.principal(), source);
        if (!found) {
            call.fail(found.error());
            return;
        }
        json items = json::array();
        for (const auto& item : found->items) {
            items.push_back(to_json(item, source));
        }
        call.ok({{"items", std::move(items)}, {"catalog_kind", catalog_kind_json(found->catalog_kind)}});
    }

    void track_detail(Call& call) {
        if (!only_params(call, {"source", "id", "context_ref"})) {
            return;
        }
        if (!call.req.has_param("source") || !call.req.has_param("id")) {
            call.fail(bad_request("source", "source and id are required"));
            return;
        }
        auto detail = services.catalog->track_detail(call.principal(), call.req.get_param_value("source"),
                                                     call.req.get_param_value("id"));
        detail ? call.ok(track_json(detail->track)) : call.fail(detail.error());
    }

    void parse_link(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"text"});
        if (!body) {
            return;
        }
        std::optional<std::string> text;
        if (!read_string(call, *body, "text", text)) {
            return;
        }
        auto parsed = services.catalog->parse_link(call.principal(), text.value_or(""));
        if (!parsed) {
            call.fail(parsed.error());
            return;
        }
        call.ok({{"matched", parsed->matched},
                 {"kind", parsed->matched ? json(parsed->kind) : json(nullptr)},
                 {"ref", optional_ref(parsed->ref)}});
    }

    static json asset_json(const application::catalog::AssetDescriptor& a) {
        return json{{"asset_id", a.asset_id},
                    {"original_track", ref_json(a.original_track)},
                    {"actual_track", ref_json(a.actual_track)},
                    {"requested_quality", domain::to_string(a.requested_quality)},
                    {"actual_quality", a.actual_quality ? json(domain::to_string(*a.actual_quality)) : json(nullptr)},
                    {"container", optional_string(a.container)},
                    {"bitrate_bps", optional_int(a.bitrate_bps)},
                    {"size_bytes", nullptr},
                    {"access_state", "available"},
                    {"completeness", domain::to_string(a.completeness)},
                    {"expires_at", utc(a.expires_at)},
                    {"warnings", warnings_json(a.warnings)}};
    }

    void resolve_asset(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"track", "context_ref", "quality", "quality_policy", "allow_preview", "fallback",
                                       "match_selection"});
        if (!body) {
            return;
        }
        const auto track = body->find("track");
        if (track == body->end() || !track->is_object() || !(*track)["source"].is_string() ||
            !(*track)["id"].is_string() || track->size() != 2) {
            call.fail(bad_request("track", "must be {source, id}"));
            return;
        }
        auto source = domain::SourceId::parse((*track)["source"].get<std::string>());
        auto id = domain::PlatformId::parse((*track)["id"].get<std::string>());
        if (!source || !id) {
            call.fail(bad_request("track", "has an invalid source or id"));
            return;
        }
        application::catalog::ResolveRequest request{call.principal(), {std::move(source).value(), std::move(id).value()},
                                                     {}, false, false};
        std::optional<std::string> quality;
        std::optional<std::string> policy;
        if (!read_string(call, *body, "quality", quality) || !read_string(call, *body, "quality_policy", policy)) {
            return;
        }
        if (quality) {
            auto parsed = domain::parse_quality(*quality);
            if (!parsed) {
                call.fail(bad_request("quality", "must be standard, high, lossless or hires"));
                return;
            }
            request.requirement.requested = parsed.value();
        }
        if (policy) {
            auto parsed = domain::parse_quality_policy(*policy);
            if (!parsed) {
                call.fail(bad_request("quality_policy", "must be strict or allow_lower"));
                return;
            }
            request.requirement.policy = parsed.value();
        }
        if (const auto preview = body->find("allow_preview"); preview != body->end()) {
            if (!preview->is_boolean()) {
                call.fail(bad_request("allow_preview", "must be a boolean"));
                return;
            }
            request.requirement.allow_preview = preview->get<bool>();
        }
        if (const auto fallback = body->find("fallback"); fallback != body->end()) {
            if (!fallback->is_object() || !(*fallback)["mode"].is_string() || fallback->size() != 1) {
                call.fail(bad_request("fallback", "must be {mode: disabled|auto}"));
                return;
            }
            const auto mode = (*fallback)["mode"].get<std::string>();
            if (mode != "disabled" && mode != "auto") {
                call.fail(bad_request("fallback.mode", "must be disabled or auto"));
                return;
            }
            request.automatic_fallback = mode == "auto";
        }
        request.match_selection = body->contains("match_selection") && !(*body)["match_selection"].is_null();
        auto resolved = services.assets->resolve(request);
        resolved ? call.ok(asset_json(resolved.value())) : call.fail(resolved.error());
    }

    void probe_asset(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"asset_id"});
        if (!body) {
            return;
        }
        std::optional<std::string> asset_id;
        if (!read_string(call, *body, "asset_id", asset_id)) {
            return;
        }
        if (!asset_id) {
            call.fail(bad_request("asset_id", "is required"));
            return;
        }
        auto report = services.assets->probe(call.principal(), *asset_id);
        if (!report) {
            call.fail(report.error());
            return;
        }
        const auto& r = report.value();
        call.ok({{"asset_id", r.asset_id},
                 {"accessible", r.accessible},
                 {"http_status", r.http_status},
                 {"content_type", optional_string(r.content_type)},
                 {"size_bytes", optional_int(r.size_bytes)},
                 {"range_supported", r.range_supported},
                 {"format", optional_string(r.format)},
                 {"estimated_duration_ms", optional_int(r.estimated_duration_ms)},
                 {"completeness", domain::to_string(r.completeness)},
                 {"warnings", warnings_json(r.warnings)}});
    }

    // --------------------------------------------------------- downloads

    // The Idempotency-Key header, or a 400 already sent.
    static std::optional<std::string> idempotency_key(const Call& call) {
        if (!call.req.has_header("Idempotency-Key")) {
            call.fail(bad_request("Idempotency-Key", "is required"));
            return std::nullopt;
        }
        auto key = call.req.get_header_value("Idempotency-Key");
        if (auto valid = application::tasks::DownloadService::check_idempotency_key(key); !valid) {
            call.fail(valid.error());
            return std::nullopt;
        }
        return key;
    }

    static void mark_replay(const Call& call, bool replayed) {
        if (replayed) {
            call.res.set_header("Idempotency-Replayed", "true");
        }
    }

    void create_download(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto key = idempotency_key(call);
        if (!key) {
            return;
        }
        auto body = json::parse(call.req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            call.fail(bad_request("body", "must be a JSON object"));
            return;
        }
        auto request = parse_download(body, "");
        if (!request) {
            call.fail(request.error());
            return;
        }
        auto created = services.downloads->create(call.principal(), request.value(), *key, body_hash(body));
        if (!created) {
            call.fail(created.error());
            return;
        }
        const auto& task = created->view.task;
        mark_replay(call, created->replayed);
        if (created->status_code == 202) {
            call.res.set_header("Location", "/api/v1/downloads/" + task.task_id);
        }
        call.ok({{"task_id", task.task_id},
                 {"status", domain::to_string(task.status)},
                 {"reused", created->reused},
                 {"links", {{"self", "/api/v1/downloads/" + task.task_id}}}},
                created->status_code);
    }

    void list_downloads(Call& call) {
        if (!only_params(call, {"status", "source", "batch_id", "limit", "cursor"})) {
            return;
        }
        application::tasks::TaskFilter filter;
        if (call.req.has_param("status")) {
            auto status = domain::parse_task_status(call.req.get_param_value("status"));
            if (!status) {
                call.fail(bad_request("status", "is not a task status"));
                return;
            }
            filter.status = status.value();
        }
        if (call.req.has_param("source")) {
            filter.source = call.req.get_param_value("source");
        }
        if (call.req.has_param("batch_id")) {
            filter.batch_id = call.req.get_param_value("batch_id");
        }
        if (call.req.has_param("cursor")) {
            filter.cursor = call.req.get_param_value("cursor");
        }
        if (!read_uint_param(call, "limit", filter.limit)) {
            return;
        }
        auto page = services.downloads->list(call.principal(), filter);
        if (!page) {
            call.fail(page.error());
            return;
        }
        json items = json::array();
        for (const auto& view : page->items) {
            items.push_back(task_json(view));
        }
        call.ok({{"items", std::move(items)},
                 {"page", {{"limit", filter.limit}, {"next_cursor", optional_string(page->next_cursor)}}}});
    }

    void get_download(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto view = services.downloads->get(call.principal(), call.req.matches[1]);
        if (!view) {
            call.fail(view.error());
            return;
        }
        const auto etag = task_etag(view->task);
        call.res.set_header("ETag", etag);
        if (call.req.get_header_value("If-None-Match") == etag) {
            call.res.status = 304;
            return;
        }
        call.ok(task_json(view.value()));
    }

    void cancel_download(Call& call) {
        if (!only_params(call, {}) || !body_object(call, {}, true)) {
            return;
        }
        auto canceled = services.downloads->cancel(call.principal(), call.req.matches[1]);
        if (!canceled) {
            call.fail(canceled.error());
            return;
        }
        call.ok(task_json(canceled->view), canceled->asynchronous ? 202 : 200);
    }

    void retry_download(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto key = idempotency_key(call);
        if (!key) {
            return;
        }
        auto body = body_object(call, {}, true);
        if (!body) {
            return;
        }
        auto retried = services.downloads->retry(call.principal(), call.req.matches[1], *key, body_hash(*body));
        if (!retried) {
            call.fail(retried.error());
            return;
        }
        const auto& task = retried->view.task;
        mark_replay(call, retried->replayed);
        call.res.set_header("Location", "/api/v1/downloads/" + task.task_id);
        call.ok({{"task_id", task.task_id},
                 {"status", domain::to_string(task.status)},
                 {"retry_of", optional_string(task.retry_of)},
                 {"links", {{"self", "/api/v1/downloads/" + task.task_id}}}},
                retried->status_code);
    }

    void create_batch(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto key = idempotency_key(call);
        if (!key) {
            return;
        }
        auto body = body_object(call, {"items"});
        if (!body) {
            return;
        }
        const auto items = body->find("items");
        if (items == body->end() || !items->is_array()) {
            call.fail(bad_request("items", "must be an array of downloads"));
            return;
        }
        std::vector<application::tasks::DownloadRequest> requests;
        for (std::size_t i = 0; i < items->size(); ++i) {
            auto request = parse_download((*items)[i], "items[" + std::to_string(i) + "].");
            if (!request) {
                call.fail(request.error());
                return;
            }
            requests.push_back(std::move(request).value());
        }
        auto created = services.downloads->create_batch(call.principal(), requests, *key, body_hash(*body));
        if (!created) {
            call.fail(created.error());
            return;
        }
        mark_replay(call, created->replayed);
        json entries = json::array();
        for (std::size_t i = 0; i < created->items.size(); ++i) {
            entries.push_back({{"index", i},
                               {"task_id", created->items[i].task.task_id},
                               {"status", domain::to_string(created->items[i].task.status)},
                               {"reused", i < created->reused.size() && created->reused[i]}});
        }
        call.ok({{"batch_id", created->batch_id},
                 {"created_count", created->created_count},
                 {"reused_count", created->reused_count},
                 {"items", std::move(entries)}},
                created->status_code);
    }

    void get_batch(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto batch = services.downloads->get_batch(call.principal(), call.req.matches[1]);
        if (!batch) {
            call.fail(batch.error());
            return;
        }
        json items = json::array();
        json stats = json::object();
        for (std::size_t i = 0; i < batch->items.size(); ++i) {
            const auto& task = batch->items[i].task;
            const auto status = std::string{domain::to_string(task.status)};
            items.push_back({{"index", i}, {"task_id", task.task_id}, {"status", status}, {"version", task.version}});
            stats[status] = stats.value(status, 0) + 1; // per entry, duplicates included
        }
        call.ok({{"batch_id", batch->batch.batch_id},
                 {"created_at", utc(batch->batch.created_at)},
                 {"created_count", batch->batch.created_count},
                 {"reused_count", batch->batch.reused_count},
                 {"items", std::move(items)},
                 {"stats", std::move(stats)}});
    }

    void cancel_batch(Call& call) {
        if (!only_params(call, {}) || !body_object(call, {}, true)) {
            return;
        }
        auto canceled = services.downloads->cancel_batch(call.principal(), call.req.matches[1]);
        if (!canceled) {
            call.fail(canceled.error());
            return;
        }
        json accepted = json::array();
        for (const auto& view : canceled->accepted) {
            accepted.push_back({{"task_id", view.task.task_id}, {"status", domain::to_string(view.task.status)}});
        }
        json refused = json::array();
        for (const auto& item : canceled->refused) {
            refused.push_back({{"task_id", item.view.task.task_id},
                               {"status", domain::to_string(item.view.task.status)},
                               {"code", domain::to_string(item.reason.code)}});
        }
        call.ok({{"accepted", std::move(accepted)}, {"refused", std::move(refused)}}, canceled->asynchronous ? 202 : 200);
    }

    // ------------------------------------------------------- credentials

    void list_credentials(Call& call) {
        if (!only_params(call, {"source"})) {
            return;
        }
        std::optional<std::string> source;
        if (call.req.has_param("source")) {
            source = call.req.get_param_value("source");
        }
        auto listed = services.credentials->list(call.principal(), source);
        if (!listed) {
            call.fail(listed.error());
            return;
        }
        json items = json::array();
        for (const auto& c : listed.value()) {
            items.push_back(credential_json(c));
        }
        call.ok({{"items", std::move(items)}});
    }

    // The platform's only cookie profile, for the {"cookie": ...} shorthand.
    domain::Result<std::string> default_cookie_profile(const std::string& source) {
        auto view = services.providers->describe(source);
        if (!view) {
            return std::move(view).error();
        }
        std::vector<std::string> cookie_profiles;
        for (const auto& method : view->auth_methods) {
            if (method.kind == ports::CredentialKind::cookie) {
                cookie_profiles.push_back(method.auth_profile);
            }
        }
        if (cookie_profiles.empty()) {
            return domain::Error{ErrorCode::capability_unsupported, "this source accepts no cookie credentials", false, {}};
        }
        if (cookie_profiles.size() > 1) {
            return domain::Error::validation("auth_profile", "this source has several cookie profiles; name one");
        }
        return cookie_profiles.front();
    }

    void create_credential(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"source", "label", "kind", "auth_profile", "secret", "cookie"});
        if (!body) {
            return;
        }
        std::optional<std::string> source;
        std::optional<std::string> label;
        std::optional<std::string> kind;
        std::optional<std::string> profile;
        std::optional<std::string> cookie;
        if (!read_string(call, *body, "source", source) || !read_string(call, *body, "label", label) ||
            !read_string(call, *body, "kind", kind) || !read_string(call, *body, "auth_profile", profile)) {
            return;
        }
        if (!source) {
            call.fail(bad_request("source", "is required"));
            return;
        }
        if (kind && *kind != "cookie" && *kind != "oauth2" && *kind != "api_key") {
            call.fail(bad_request("kind", "must be cookie, oauth2 or api_key"));
            return;
        }
        const bool shorthand = body->contains("cookie");
        if (!shorthand && (!kind || !profile || !body->contains("secret"))) {
            call.fail(bad_request("secret", "kind, auth_profile and secret are required unless cookie is given"));
            return;
        }
        const auto kind_text = kind.value_or("cookie");
        if (!read_credential_secret(call, *body, kind_text, cookie)) {
            return;
        }
        if (!cookie) {
            call.fail(bad_request("secret", "is required"));
            return;
        }
        if (!profile) {
            auto found = default_cookie_profile(*source);
            if (!found) {
                call.fail(found.error());
                return;
            }
            profile = std::move(found).value();
        }
        const auto credential_kind = kind_text == "oauth2" ? ports::CredentialKind::oauth2
                                   : kind_text == "api_key" ? ports::CredentialKind::api_key
                                                            : ports::CredentialKind::cookie;
        auto created = services.credentials->create({call.principal(), *source, label.value_or(""),
                                                     credential_kind, *profile,
                                                     ports::SecretBytes{std::move(*cookie)}, std::nullopt});
        if (!created) {
            call.fail(created.error());
            return;
        }
        call.ok(credential_json(created.value()), 201);
    }

    void update_credential(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"label", "secret", "cookie"});
        if (!body) {
            return;
        }
        std::optional<std::string> label;
        std::optional<std::string> cookie;
        auto current = services.credentials->get(call.principal(), call.req.matches[1]);
        if (!current) { call.fail(current.error()); return; }
        if (!read_string(call, *body, "label", label) ||
            !read_credential_secret(call, *body, current->kind, cookie)) {
            return;
        }
        accounts::CredentialUpdate update;
        update.expected_version = current->version;
        update.label = std::move(label);
        if (cookie) {
            update.secret = ports::SecretBytes{std::move(*cookie)};
        }
        auto updated = services.credentials->update(call.principal(), call.req.matches[1], std::move(update));
        if (!updated) {
            call.fail(updated.error());
            return;
        }
        call.ok(credential_json(updated.value()));
    }

    void verify_credential(Call& call) {
        if (!only_params(call, {}) || !body_object(call, {}, true)) {
            return;
        }
        const ports::OperationContext context{SteadyClock::now() + options.catalog.total_budget, {}};
        auto verified = services.credentials->verify(call.principal(), call.req.matches[1], context);
        if (!verified) {
            call.fail(verified.error());
            return;
        }
        const auto status = verified->verification.status;
        call.ok({{"result", status == ports::VerificationStatus::valid     ? "valid"
                            : status == ports::VerificationStatus::invalid ? "invalid"
                                                                           : "unknown"},
                 {"account_hint", optional_string(verified->verification.account_hint)},
                 {"credential", credential_json(verified->credential)}});
    }

    void revoke_credential(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto revoked = services.credentials->revoke(call.principal(), call.req.matches[1]);
        if (!revoked) {
            call.fail(revoked.error());
            return;
        }
        call.ok(credential_json(revoked.value()));
    }

    void get_default(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto selection = services.credentials->get_default(call.principal(), call.req.matches[1]);
        selection ? call.ok(selection_json(selection.value())) : call.fail(selection.error());
    }

    void set_default(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"credential_id"});
        if (!body) {
            return;
        }
        std::optional<std::string> credential_id;
        if (!read_string(call, *body, "credential_id", credential_id)) {
            return;
        }
        if (!credential_id) {
            call.fail(bad_request("credential_id", "is required"));
            return;
        }
        auto selection = services.credentials->set_default(call.principal(), call.req.matches[1], *credential_id);
        if (!selection) {
            // The request is well-formed; a credential of another source, revoked or
            // otherwise unusable is an invalid selection (422), not a bad request.
            auto error = from_domain(selection.error());
            if (selection.error().code == ErrorCode::validation_failed) {
                error.status = 422;
            }
            call.fail(error);
            return;
        }
        call.ok(selection_json(selection.value()));
    }

    void clear_default(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto selection = services.credentials->clear_default(call.principal(), call.req.matches[1]);
        selection ? call.ok(selection_json(selection.value())) : call.fail(selection.error());
    }
};

ControlApi::ControlApi(ControlApiOptions options, ControlApiServices services)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(services))) {}

ControlApi::~ControlApi() { stop(); }

domain::Result<std::uint16_t> ControlApi::bind() {
    const auto& listener = impl_->options.listener;
    int bound = -1;
    if (listener.port == 0) {
        bound = impl_->server.bind_to_any_port(listener.host);
    } else if (impl_->server.bind_to_port(listener.host, listener.port)) {
        bound = listener.port;
    }
    if (bound <= 0) {
        domain::Error error{ErrorCode::service_busy, "the control listener could not bind", false, {}};
        error.details.push_back({"control", listener.host + ":" + std::to_string(listener.port) +
                                                " is in use or not available"});
        return error;
    }
    impl_->port = static_cast<std::uint16_t>(bound);
    return impl_->port.load();
}

void ControlApi::start() {
    impl_->thread = std::thread([this] { impl_->server.listen_after_bind(); });
    impl_->server.wait_until_ready();
}

void ControlApi::stop() {
    if (!impl_) {
        return;
    }
    std::call_once(impl_->stopped, [this] {
        impl_->server.stop();
        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }
    });
}

std::uint16_t ControlApi::port() const noexcept { return impl_->port.load(); }

} // namespace music_service::api
