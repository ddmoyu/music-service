#pragma once
#include "providers/qq/client.hpp"
namespace music_service::providers::qq::parsing {
std::string text(const Json& j, const char* key, bool required = false);
std::uint64_t number(const Json& j, const char* key);
domain::PlatformRef ref(std::string value, bool numeric = false);
domain::Artwork artwork(std::string value);
domain::Track track(const Json& j);
domain::Playlist playlist(const Json& j);
domain::Album album(const Json& j);
domain::Artist artist(const Json& j);
domain::Chart chart(const Json& j);
}
