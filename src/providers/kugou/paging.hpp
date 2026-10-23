#pragma once
#include "providers/kugou/client.hpp"
#include <algorithm>

namespace music_service::providers::kugou {
template <class Item>
ports::CatalogPage<Item> page(std::vector<Item> values, std::uint32_t start, std::uint32_t limit,
                              std::uint64_t total, std::string rev = {}) {
    if (total > 10000000 || start > total || values.size() > limit || values.size() > total - start)
        throw std::invalid_argument{"page bounds"};
    const auto consumed = std::min<std::uint64_t>(limit, total - start);
    const bool more = start + consumed < total;
    // 触及实现边界必须明确失败，不能把尚有条目的目录标成已遍历完毕。
    if (more && start + consumed > 10000)
        throw Error{ErrorCode::upstream_failed,
                    "Kugou pagination exceeds the supported offset",
                    false,
                    {{"pagination", "maximum offset is 10000"}}};
    ports::CatalogPage<Item> result;
    result.page.items = std::move(values);
    result.page.page = {limit, static_cast<std::uint32_t>(result.page.items.size()), total, more};
    // 上游按请求窗口推进；短页未提供缺失 ID，不能编造 unavailable_count。
    if (more)
        result.next_cursor = std::to_string(start + consumed) + (rev.empty() ? "" : ":" + rev);
    return result;
}
inline std::string window(std::uint32_t start, std::uint32_t limit) {
    return "&page=" + std::to_string(start / limit + 1) + "&pagesize=" + std::to_string(limit);
}
} // namespace music_service::providers::kugou
