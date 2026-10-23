#pragma once
#include "providers/kugou/client.hpp"
namespace music_service::providers::kugou::parsing {
std::string text(const Json& j, const char* key, bool required = false);
std::string first(const Json& j, std::initializer_list<const char*> keys);
std::uint64_t number(const Json& j, const char* key);
void array(const Json& j, std::size_t max);
domain::PlatformRef ref(std::string id, bool track = false);
domain::Artwork artwork(std::string value, bool artist = false);
domain::Track track(const Json& j, bool detail = false);
domain::Playlist playlist(const Json& j);
domain::Album album(const Json& j);
domain::Chart chart(const Json& j);
domain::Artist artist(const Json& j, bool search = false);
} // namespace music_service::providers::kugou::parsing
