#pragma once
#include "providers/migu/client.hpp"
namespace music_service::providers::migu::parsing {
std::string text(const Json&, const char* key, bool required = false);
std::string first(const Json&, std::initializer_list<const char*> keys);
std::optional<std::uint64_t> number(const Json&, const char* key);
std::uint64_t count(const Json&, const char* key);
domain::PlatformRef ref(std::string id, bool album = false);
domain::Artwork artwork(std::string url);
domain::Track track(const Json&);
domain::Playlist playlist(const Json&);
domain::Album album(const Json&);
domain::Artist artist(const Json&);
domain::Chart chart(const Json&);
} // namespace music_service::providers::migu::parsing
