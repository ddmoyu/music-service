#pragma once

#include "application/accounts/credential_service.hpp"
#include "application/catalog/handle_table.hpp"
#include "application/catalog/result_cache.hpp"
#include "application/config/service_config.hpp"
#include "application/ports/providers/directory.hpp"
#include "application/ports/runtime/clock.hpp"
#include "application/ports/runtime/executor.hpp"
#include "domain/catalog/track.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace music_service::application::catalog {

struct CatalogLimits {
    config::Catalog catalog;
    config::Cache cache;
    config::Handles handles;
    config::Lifetimes lifetimes;
    std::size_t per_owner_handle_share{4}; // a principal may hold 1/share of each handle table
};

struct SearchRequest {
    std::string owner_id;
    std::string q;
    // Explicit sources, "all", or nothing for catalog.default_sources.
    std::vector<std::string> sources;
    bool all_sources{false};
    std::uint32_t limit{20};
    std::optional<std::string> cursor; // exactly one source
    std::optional<std::chrono::milliseconds> timeout;
};

enum class SourceStatus { ok, failed, unsupported, disabled };

struct SourcePage {
    std::uint32_t limit{0};
    std::uint32_t returned{0};
    std::optional<std::uint64_t> total;
    std::optional<bool> has_more;
    std::optional<std::string> next_cursor; // service cursor handle
};

struct CacheInfo {
    bool hit{false};
    std::chrono::system_clock::time_point created_at;
};

struct SourceResult {
    std::string source;
    SourceStatus status{SourceStatus::failed};
    std::chrono::milliseconds elapsed{0};
    std::optional<SourcePage> page;
    std::optional<domain::Error> error;
    std::optional<CacheInfo> cache;
    std::vector<domain::Track> items; // moved into SearchResult::items
};

struct SearchResult {
    std::vector<domain::Track> items; // grouped by source in request order, upstream order within
    std::vector<SourceResult> sources;
    bool partial{false};
    // Set when no source succeeded: every executed source timed out (UPSTREAM_TIMEOUT),
    // other failures (UPSTREAM_FAILED), or nothing selected is usable
    // (CAPABILITY_UNSUPPORTED). The per-source results stay available.
    std::optional<domain::Error> failure;
};

struct LinkMatch {
    bool matched{false};
    std::string kind;
    std::optional<domain::PlatformRef> ref;
};

// Search, track details and link parsing (feature-modules.md, 搜索). Every platform
// call runs under the caller's account binding for that source: the cache, in-flight
// merging and cursors are keyed by the full AuthIdentityKey, and cursors made under
// the owner's default selection also carry its selection_version, so switching
// accounts (even A -> B -> A) expires them. Failures are never cached.
class CatalogService {
public:
    static constexpr std::size_t max_query_chars = 200;
    static constexpr std::size_t max_link_text_chars = 4096;

    CatalogService(CatalogLimits limits, std::shared_ptr<const ports::ProviderDirectory> providers,
                   std::shared_ptr<accounts::CredentialService> credentials,
                   std::shared_ptr<ports::WorkExecutor> executor, std::shared_ptr<const ports::Clock> clock,
                   std::function<std::string()> new_cursor_id);

    // Validation and cursor problems are errors; per-source outcomes are in the result.
    domain::Result<SearchResult> search_tracks(const SearchRequest& request);

    struct Detail {
        domain::Track track;
        CacheInfo cache;
    };
    domain::Result<Detail> track_detail(const std::string& owner_id, const std::string& source,
                                        const std::string& id);

    // One entity from share text, or matched=false. Several distinct entities are
    // VALIDATION_FAILED. Never touches the network.
    domain::Result<LinkMatch> parse_link(const std::string& owner_id, const std::string& text);

private:
    struct CursorState;
    struct Plan;

    SourceResult run_search(const SearchRequest& request, const std::string& source,
                            const std::optional<CursorState>& cursor, std::chrono::steady_clock::time_point deadline,
                            std::stop_token stop);
    domain::Result<std::vector<std::string>> select_sources(const SearchRequest& request);

    CatalogLimits limits_;
    std::shared_ptr<const ports::ProviderDirectory> providers_;
    std::shared_ptr<accounts::CredentialService> credentials_;
    std::shared_ptr<ports::WorkExecutor> executor_;
    std::shared_ptr<const ports::Clock> clock_;

    ResultCache<ports::TrackSearchPage> search_cache_;
    ResultCache<domain::Track> detail_cache_;
    SingleFlight<ports::TrackSearchPage> search_flights_;
    SingleFlight<domain::Track> detail_flights_;
    std::shared_ptr<HandleTable<CursorState>> cursors_;
};

} // namespace music_service::application::catalog
