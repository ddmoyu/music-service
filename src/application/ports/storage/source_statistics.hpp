#pragma once

#include "domain/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace music_service::application::ports {

struct SourceDownloadStatistics {
    std::string source;
    std::uint64_t successes{0};
    std::uint64_t failures{0};

    // Beta(1,1) 先验，避免一次成功就获得 100% 的排序权重。
    [[nodiscard]] double score() const {
        return (static_cast<double>(successes) + 1.0) /
               (static_cast<double>(successes) + static_cast<double>(failures) + 2.0);
    }
};

class SourceStatisticsRepository {
public:
    virtual ~SourceStatisticsRepository() = default;
    virtual domain::Result<std::vector<SourceDownloadStatistics>> source_statistics(const std::string& owner) = 0;
};

} // namespace music_service::application::ports
