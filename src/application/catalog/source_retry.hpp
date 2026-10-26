#pragma once

#include "application/accounts/credential_service.hpp"
#include "application/ports/storage/source_statistics.hpp"
#include <memory>

namespace music_service::application::catalog {

// 持久化身份只含公开元数据；账号仍由下载意图固定，URL 和秘密不进入快照。
struct SourceRetryState {
    std::optional<domain::Track> original;
    std::vector<std::string> attempted_sources;
    bool pending{false};
};

bool strict_track_match(const domain::Track& original, const domain::Track& candidate);
bool source_failure(domain::ErrorCode code);
std::string encode_source_retry(const SourceRetryState& state);
domain::Result<SourceRetryState> decode_source_retry(const std::optional<std::string>& execution,
                                                    const domain::TrackRef& original);

class SourceRetryService {
public:
    static constexpr std::size_t max_sources = 32;
    static constexpr std::uint32_t search_limit = 10;
    SourceRetryService(std::shared_ptr<const ports::ProviderDirectory> providers,
                       std::shared_ptr<accounts::CredentialService> credentials,
                       std::shared_ptr<ports::SourceStatisticsRepository> statistics = {});

    // 原平台保留显式绑定，其他平台在接单时固定账号，不在执行中更换同平台账号。
    domain::Result<std::vector<domain::AccountBinding>> bindings(
        const std::string& owner, const std::optional<domain::AccountBinding>& original = {});
    domain::Result<domain::Track> identity(const std::string& owner, const domain::TrackRef& track,
                                         ports::OperationContext operation);
    domain::Result<domain::Track> next(const std::string& owner, const domain::Track& original,
        const std::vector<domain::AccountBinding>& bindings, std::vector<std::string>& attempted,
        ports::OperationContext operation);
    domain::Result<std::vector<ports::SourceDownloadStatistics>> statistics(const std::string& owner);

private:
    domain::Result<ports::ProviderContext> context(const std::string& owner, const std::string& source,
                                                  const std::string& capability, ports::OperationContext operation);
    std::shared_ptr<const ports::ProviderDirectory> providers_;
    std::shared_ptr<accounts::CredentialService> credentials_;
    std::shared_ptr<ports::SourceStatisticsRepository> statistics_;
};

} // namespace music_service::application::catalog
