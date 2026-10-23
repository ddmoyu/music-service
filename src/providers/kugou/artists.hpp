#pragma once
#include "providers/kugou/client.hpp"

namespace music_service::providers::kugou {
// 以官网公开的 H5 签名算法构造匿名搜索请求；时间入参便于固定向量验证。
std::string artist_search_url(std::string_view query, std::uint32_t page, std::uint32_t limit,
                              std::uint64_t timestamp_ms);
void add_artists(ports::ProviderBundle& bundle);
} // namespace music_service::providers::kugou
