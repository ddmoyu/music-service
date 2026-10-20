#include "application/catalog/catalog_service.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <utility>

namespace music_service::application::catalog {

namespace {

using domain::Error;
using domain::ErrorCode;
using SteadyClock = std::chrono::steady_clock;

constexpr std::string_view search_capability = "search_tracks";

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n\f\v") - first + 1);
}

std::size_t utf8_chars(std::string_view text) {
    return static_cast<std::size_t>(
        std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// Unambiguous cache and flight keys: length-prefixed parts.
std::string key_of(std::initializer_list<std::string_view> parts) {
    std::string key;
    for (const auto part : parts) {
        key += std::to_string(part.size());
        key.push_back(':');
        key.append(part);
        key.push_back(';');
    }
    return key;
}

std::size_t track_bytes(const domain::Track& track) {
    std::size_t bytes = 256 + track.title.size() + track.ref.id.value().size();
    for (const auto& artist : track.artists) {
        bytes += 64 + artist.name.size();
    }
    if (track.album) {
        bytes += 64 + track.album->title.size();
    }
    return bytes;
}

bool is_timeout(const Error& error) {
    return error.code == ErrorCode::upstream_timeout || error.code == ErrorCode::deadline_exceeded;
}

// Only sanitized errors leave a source; internal details stay internal.
Error public_error(Error error) {
    if (error.code == ErrorCode::internal) {
        return Error{ErrorCode::internal, "internal error", false, {}};
    }
    return error;
}

std::shared_ptr<const ports::SecretBytes> share_secret(std::optional<ports::SecretBytes>& secret) {
    if (!secret) {
        return nullptr;
    }
    return std::make_shared<const ports::SecretBytes>(std::move(*secret));
}

} // namespace

struct CatalogService::CursorState {
    std::string source;
    std::string query;
    std::uint32_t limit{0};
    std::string identity;                         // AuthIdentityKey::canonical()
    std::optional<std::uint64_t> selection_version; // when the default selection was used
    std::string implementation;
    std::string provider_cursor;
};

CatalogService::CatalogService(CatalogLimits limits, std::shared_ptr<const ports::ProviderDirectory> providers,
                               std::shared_ptr<accounts::CredentialService> credentials,
                               std::shared_ptr<ports::WorkExecutor> executor, std::shared_ptr<const ports::Clock> clock,
                               std::function<std::string()> new_cursor_id)
    : limits_(std::move(limits)), providers_(std::move(providers)), credentials_(std::move(credentials)),
      executor_(std::move(executor)), clock_(std::move(clock)),
      search_cache_(limits_.cache.catalog_entries, static_cast<std::size_t>(limits_.cache.catalog_bytes / 2),
                    limits_.cache.catalog_ttl),
      detail_cache_(limits_.cache.catalog_entries, static_cast<std::size_t>(limits_.cache.catalog_bytes / 2),
                    limits_.cache.catalog_ttl),
      cursors_(std::make_shared<HandleTable<CursorState>>(
          limits_.handles.cursors,
          std::max<std::size_t>(1, limits_.handles.cursors / std::max<std::size_t>(1, limits_.per_owner_handle_share)),
          std::move(new_cursor_id))) {}

domain::Result<std::vector<std::string>> CatalogService::select_sources(const SearchRequest& request) {
    std::vector<std::string> sources;
    if (request.all_sources) {
        ports::ProviderListQuery query{request.owner_id, std::string{search_capability}, true, 100, std::nullopt};
        while (true) {
            auto page = providers_->list(query);
            if (!page) {
                return std::move(page).error();
            }
            for (const auto& view : page->items) {
                sources.push_back(view.source.value());
            }
            if (!page->next_cursor) {
                break;
            }
            query.cursor = page->next_cursor;
        }
    } else if (request.sources.empty()) {
        sources = limits_.catalog.default_sources;
    } else {
        std::set<std::string> seen;
        for (const auto& source : request.sources) {
            if (auto known = providers_->check_source(source); !known) {
                auto error = known.error();
                error.details.push_back({"sources", source + " is not a registered source"});
                return error;
            }
            if (seen.insert(source).second) {
                sources.push_back(source);
            }
        }
    }
    if (sources.size() > limits_.catalog.max_sources_per_request) {
        return Error::validation("sources", std::to_string(sources.size()) + " sources exceed the limit of " +
                                                std::to_string(limits_.catalog.max_sources_per_request));
    }
    return sources;
}

domain::Result<SearchResult> CatalogService::search_tracks(const SearchRequest& request) {
    const auto query = trim(request.q);
    if (query.empty()) {
        return Error::validation("q", "is required");
    }
    if (utf8_chars(query) > max_query_chars) {
        return Error::validation("q", "must be at most 200 characters");
    }
    if (request.limit < 1 || request.limit > 50) {
        return Error::validation("limit", "must be between 1 and 50");
    }
    const auto timeout = request.timeout.value_or(limits_.catalog.total_budget);
    if (timeout < std::chrono::milliseconds{1000} || timeout > limits_.catalog.max_budget) {
        return Error::validation("timeout_ms", "must be between 1000 and " +
                                                   std::to_string(limits_.catalog.max_budget.count()));
    }

    std::optional<CursorState> cursor;
    if (request.cursor) {
        if (request.all_sources || request.sources.size() != 1) {
            return Error::validation("cursor", "continues exactly one source");
        }
        auto found = cursors_->get(request.owner_id, *request.cursor, clock_->now(), ErrorCode::cursor_expired);
        if (!found) {
            // Unknown, foreign or lost with a restart: the client starts again.
            return Error{ErrorCode::cursor_expired, "the cursor has expired", false, {}};
        }
        cursor = std::move(found->first);
        if (cursor->source != request.sources.front() || cursor->query != query || cursor->limit != request.limit) {
            return Error{ErrorCode::cursor_mismatch, "the cursor belongs to another source or query", false, {}};
        }
    }

    auto selected = select_sources(request);
    if (!selected) {
        return std::move(selected).error();
    }
    const auto& sources = selected.value();
    SearchResult result;
    if (sources.empty()) {
        result.failure = Error{ErrorCode::capability_unsupported, "no source is selected for search", false, {}};
        return result;
    }

    // Bounded fan-out: at most catalog.fanout sources at a time on the shared pool; the
    // caller waits until all report or the total deadline passes. Late results land in
    // the shared state and are ignored.
    struct Shared {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<std::optional<SourceResult>> results;
        std::deque<std::size_t> waiting;
        std::size_t running{0};
        std::stop_source stop;
    };
    auto shared = std::make_shared<Shared>();
    shared->results.resize(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        shared->waiting.push_back(i);
    }
    const auto started = SteadyClock::now();
    const auto deadline = started + timeout;
    const auto per_source = std::min(deadline, started + limits_.catalog.per_source_budget);
    const auto fanout = std::max<std::uint32_t>(1, limits_.catalog.fanout);
    const SearchRequest copy{request.owner_id,  std::string{query}, request.sources, request.all_sources,
                             request.limit,     request.cursor,     request.timeout};

    // Starts queued sources while fewer than fanout run; called with the lock held.
    std::function<void()> launch;
    launch = [this, shared, &sources, copy, cursor, per_source, fanout]() {
        while (shared->running < fanout && !shared->waiting.empty()) {
            const auto index = shared->waiting.front();
            shared->waiting.pop_front();
            ++shared->running;
            auto submitted = executor_->submit([this, shared, source = sources[index], index, copy, cursor, per_source,
                                                stop = shared->stop.get_token()](std::stop_token) {
                auto outcome = run_search(copy, source, cursor, per_source, stop);
                const std::lock_guard lock{shared->mutex};
                if (!shared->results[index]) {
                    shared->results[index] = std::move(outcome);
                }
                --shared->running;
                shared->changed.notify_all();
            });
            if (!submitted) {
                --shared->running;
                SourceResult busy;
                busy.source = sources[index];
                busy.error = submitted.error();
                shared->results[index] = std::move(busy);
            }
        }
    };

    {
        std::unique_lock lock{shared->mutex};
        launch();
        while (true) {
            const bool done = std::all_of(shared->results.begin(), shared->results.end(),
                                          [](const auto& r) { return r.has_value(); });
            if (done) {
                break;
            }
            if (shared->changed.wait_until(lock, deadline) == std::cv_status::timeout) {
                break;
            }
            launch();
        }
        // Whatever has not answered by now timed out; its work is asked to stop.
        shared->stop.request_stop();
        shared->waiting.clear();
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (!shared->results[i]) {
                SourceResult late;
                late.source = sources[i];
                late.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - started);
                late.error = Error{ErrorCode::upstream_timeout, "the source did not answer in time", true, {}};
                shared->results[i] = std::move(late);
            }
        }
        for (auto& r : shared->results) {
            auto& items = r->items;
            result.items.insert(result.items.end(), std::make_move_iterator(items.begin()),
                                std::make_move_iterator(items.end()));
            items.clear();
            result.sources.push_back(std::move(*r));
        }
    }
    // The fan-out lambda refers to locals; make sure no later launch can run.
    launch = nullptr;

    bool any_ok = false;
    bool any_executed = false;
    bool all_timeouts = true;
    for (const auto& source : result.sources) {
        if (source.status == SourceStatus::ok) {
            any_ok = true;
        } else if (source.status == SourceStatus::failed) {
            any_executed = true;
            all_timeouts = all_timeouts && source.error && is_timeout(*source.error);
        }
    }
    if (cursor && !any_ok && result.sources.front().error &&
        result.sources.front().error->code == ErrorCode::cursor_expired) {
        return *result.sources.front().error;
    }
    result.partial = any_ok && std::any_of(result.sources.begin(), result.sources.end(),
                                           [](const SourceResult& s) { return s.status != SourceStatus::ok; });
    if (!any_ok) {
        if (!any_executed) {
            result.failure = Error{ErrorCode::capability_unsupported, "no selected source supports this search", false, {}};
        } else if (all_timeouts) {
            result.failure = Error{ErrorCode::upstream_timeout, "every source timed out", true, {}};
        } else {
            result.failure = Error{ErrorCode::upstream_failed, "every source failed", true, {}};
        }
    }
    return result;
}

SourceResult CatalogService::run_search(const SearchRequest& request, const std::string& source,
                                        const std::optional<CursorState>& cursor,
                                        std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
    const auto started = SteadyClock::now();
    SourceResult outcome;
    outcome.source = source;
    const auto finish = [&](SourceResult& r) -> SourceResult {
        r.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - started);
        return std::move(r);
    };
    const auto fail = [&](Error error) {
        outcome.status = SourceStatus::failed;
        outcome.error = public_error(std::move(error));
        return finish(outcome);
    };

    if (auto spec = providers_->describe_capability(source, search_capability); !spec) {
        const auto code = spec.error().code;
        outcome.status = code == ErrorCode::provider_disabled        ? SourceStatus::disabled
                         : code == ErrorCode::capability_unsupported ? SourceStatus::unsupported
                                                                     : SourceStatus::failed;
        outcome.error = spec.error();
        return finish(outcome);
    }
    auto binding = credentials_->resolve_binding(request.owner_id, source, search_capability, std::nullopt);
    if (!binding) {
        return fail(std::move(binding).error());
    }
    auto lease = credentials_->lease(binding->binding);
    if (!lease) {
        return fail(std::move(lease).error());
    }
    const auto identity = lease->identity.canonical();
    if (cursor && (cursor->identity != identity || cursor->selection_version != binding->selection_version)) {
        return fail(Error{ErrorCode::cursor_expired, "the account behind the cursor changed", false, {}});
    }
    auto view = providers_->describe(source);
    if (!view) {
        return fail(std::move(view).error());
    }
    const auto implementation = view->implementation_id + "@" + view->implementation_version;
    if (cursor && cursor->implementation != implementation) {
        return fail(Error{ErrorCode::cursor_expired, "the source implementation changed", false, {}});
    }
    const auto provider_cursor = cursor ? std::optional<std::string>{cursor->provider_cursor} : std::nullopt;
    const auto key = key_of({search_capability, identity, implementation, request.q, std::to_string(request.limit),
                             provider_cursor.value_or("")});

    std::shared_ptr<const ports::TrackSearchPage> page;
    const auto now = clock_->now();
    if (auto hit = search_cache_.get(key, now)) {
        page = hit->value;
        outcome.cache = CacheInfo{true, hit->created_at};
    } else {
        auto secret = share_secret(lease->secret);
        auto fetched = search_flights_.join(key, deadline, [&]() -> SingleFlight<ports::TrackSearchPage>::Result {
            if (stop.stop_requested()) {
                return Error{ErrorCode::canceled, "the request was canceled", false, {}};
            }
            auto search = providers_->track_search(source);
            if (!search) {
                return std::move(search).error();
            }
            const ports::ProviderContext context{{deadline, stop}, lease->identity, std::nullopt, secret};
            auto found = search.value()->search_tracks(context, {request.q, request.limit, provider_cursor});
            if (!found) {
                return std::move(found).error();
            }
            return std::make_shared<const ports::TrackSearchPage>(std::move(found).value());
        });
        if (!fetched) {
            return fail(std::move(fetched).error());
        }
        page = std::move(fetched).value();
        std::size_t bytes = 128;
        for (const auto& track : page->page.items) {
            bytes += track_bytes(track);
        }
        search_cache_.put(key, page, bytes, now);
        outcome.cache = CacheInfo{false, now};
    }

    SourcePage source_page{request.limit, page->page.page.returned, page->page.page.total, page->page.page.has_more,
                           std::nullopt};
    if (page->next_cursor) {
        auto handle = cursors_->put(request.owner_id,
                                    CursorState{source, request.q, request.limit, identity, binding->selection_version,
                                                implementation, *page->next_cursor},
                                    now + limits_.lifetimes.cursor, now);
        if (handle) {
            source_page.next_cursor = std::move(handle).value();
        } else {
            source_page.has_more = page->page.page.has_more; // the page is still valid; no cursor to continue
        }
    }
    outcome.status = SourceStatus::ok;
    outcome.page = std::move(source_page);
    outcome.items = page->page.items;
    return finish(outcome);
}

domain::Result<CatalogService::Detail> CatalogService::track_detail(const std::string& owner_id,
                                                                   const std::string& source, const std::string& id) {
    auto platform_id = domain::PlatformId::parse(id);
    if (!platform_id) {
        return std::move(platform_id).error();
    }
    auto source_id = domain::SourceId::parse(source);
    if (!source_id) {
        return std::move(source_id).error();
    }
    if (auto known = providers_->check_source(source); !known) {
        return known.error();
    }
    auto binding = credentials_->resolve_binding(owner_id, source, "track_detail", std::nullopt);
    if (!binding) {
        return std::move(binding).error();
    }
    auto lease = credentials_->lease(binding->binding);
    if (!lease) {
        return std::move(lease).error();
    }
    auto view = providers_->describe(source);
    if (!view) {
        return std::move(view).error();
    }
    const auto identity = lease->identity.canonical();
    const auto key = key_of({"track_detail", identity, view->implementation_id + "@" + view->implementation_version, id});
    const auto now = clock_->now();
    if (auto hit = detail_cache_.get(key, now)) {
        return Detail{*hit->value, CacheInfo{true, hit->created_at}};
    }
    const auto deadline = SteadyClock::now() + limits_.catalog.total_budget;
    auto secret = share_secret(lease->secret);
    domain::TrackRef ref{std::move(source_id).value(), std::move(platform_id).value()};
    auto fetched = detail_flights_.join(key, deadline, [&]() -> SingleFlight<domain::Track>::Result {
        auto detail = providers_->track_detail(source);
        if (!detail) {
            return std::move(detail).error();
        }
        const ports::ProviderContext context{{deadline, {}}, lease->identity, std::nullopt, secret};
        auto track = detail.value()->track_detail(context, ref);
        if (!track) {
            return std::move(track).error();
        }
        return std::make_shared<const domain::Track>(std::move(track).value());
    });
    if (!fetched) {
        return public_error(std::move(fetched).error());
    }
    detail_cache_.put(key, fetched.value(), track_bytes(*fetched.value()), now);
    return Detail{*fetched.value(), CacheInfo{false, now}};
}

namespace {

// Share text separators: whitespace, ASCII brackets and quotes, and the CJK
// punctuation that commonly surrounds pasted links.
std::vector<std::string> link_candidates(std::string_view text) {
    static constexpr std::string_view cjk[] = {"，", "。", "、", "：", "；", "！", "？", "（", "）",
                                               "【", "】", "《", "》", "「", "」", "“", "”"};
    std::string normalized{text};
    for (const auto mark : cjk) {
        for (auto at = normalized.find(mark); at != std::string::npos; at = normalized.find(mark, at)) {
            normalized.replace(at, mark.size(), " ");
        }
    }
    for (auto& c : normalized) {
        if (std::string_view{"\t\r\n<>\"'()[]{}"}.find(c) != std::string_view::npos) {
            c = ' ';
        }
    }
    std::vector<std::string> candidates;
    std::size_t at = 0;
    while (at < normalized.size()) {
        const auto start = normalized.find_first_not_of(' ', at);
        if (start == std::string::npos) {
            break;
        }
        const auto end = normalized.find(' ', start);
        auto token = std::string_view{normalized}.substr(start, end == std::string::npos ? std::string::npos : end - start);
        at = end == std::string::npos ? normalized.size() : end;
        const auto scheme = std::min(token.find("http://"), token.find("https://"));
        if (scheme == std::string_view::npos) {
            continue;
        }
        token.remove_prefix(scheme);
        while (!token.empty() && std::string_view{".,;:!?"}.find(token.back()) != std::string_view::npos) {
            token.remove_suffix(1);
        }
        candidates.emplace_back(token);
    }
    return candidates;
}

} // namespace

domain::Result<LinkMatch> CatalogService::parse_link(const std::string& owner_id, const std::string& text) {
    if (trim(text).empty()) {
        return Error::validation("text", "is required");
    }
    if (utf8_chars(text) > max_link_text_chars) {
        return Error::validation("text", "must be at most 4096 characters");
    }
    const auto candidates = link_candidates(text);
    std::vector<std::shared_ptr<ports::ILinkParser>> parsers;
    ports::ProviderListQuery query{owner_id, "parse_links", true, 100, std::nullopt};
    while (!candidates.empty()) {
        auto page = providers_->list(query);
        if (!page) {
            return std::move(page).error();
        }
        for (const auto& view : page->items) {
            if (auto parser = providers_->link_parser(view.source.value())) {
                parsers.push_back(std::move(parser).value());
            }
        }
        if (!page->next_cursor) {
            break;
        }
        query.cursor = page->next_cursor;
    }
    std::set<std::pair<std::string, domain::PlatformRef>> found;
    for (const auto& candidate : candidates) {
        for (const auto& parser : parsers) {
            if (auto parsed = parser->parse_link(candidate)) {
                found.emplace(parsed->kind, parsed->ref);
            }
        }
    }
    if (found.empty()) {
        return LinkMatch{};
    }
    if (found.size() > 1) {
        return Error::validation("text", "names more than one entity; send one link");
    }
    return LinkMatch{true, found.begin()->first, found.begin()->second};
}

} // namespace music_service::application::catalog
