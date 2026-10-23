#pragma once
#include "providers/qianqian/client.hpp"

namespace music_service::providers::qianqian::parsing {
std::string text(const Json& json, const char* key, bool required = false);
std::uint64_t number(const Json& json, const char* key, std::uint64_t maximum = 10000000);
bool more(const Json& json);
void array(const Json& json, std::size_t maximum);
domain::PlatformRef ref(std::string id, char prefix);
std::string resource_url(std::string value, bool audio = false);
domain::Track track(const Json& json);
domain::Playlist playlist(const Json& json);
domain::Album album(const Json& json);
Json song(const Client& client, const ports::ProviderContext& context, const domain::TrackRef& ref, std::string_view operation);
}
