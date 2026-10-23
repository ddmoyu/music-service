#pragma once
#include "providers/jamendo/client.hpp"

namespace music_service::providers::jamendo::parsing {
std::string text(const Json& object, const char* key, bool required = false);
std::string clean_text(std::string_view value);
std::uint64_t number(const Json& object, const char* key);
std::string id(const Json& object, const char* key = "id");
domain::PlatformRef ref(std::string value);
bool available(const Json& object);
domain::Artwork artwork(const Json& object);
domain::Track track(const Json& object);
domain::Album album(const Json& object);
domain::Artist artist(const Json& object);
domain::Playlist playlist(const Json& object);
std::vector<domain::Track> tracks(const Client& client, const ports::ProviderContext& context,
    std::string_view operation, const std::vector<Json>& rows, const Json& known_album = Json());
domain::Album album_detail(const Client& client, const ports::ProviderContext& context,
    std::string_view operation, const Json& object);
// 集合修订只含顺序、身份和位置，播放量变化不会使游标失效。
Json members(const Json& object, const char* field, bool positions);
}
