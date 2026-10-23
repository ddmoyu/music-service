#pragma once

#include "application/ports/providers/provider.hpp"

namespace music_service::providers::kuwo::parsing {

domain::Result<application::ports::CatalogPage<domain::Artist>> parse_artist_search(
    std::string_view body, std::uint32_t page, std::uint32_t limit);
domain::Result<domain::Artist> parse_artist_detail(std::string_view body, std::string_view id);
domain::Result<application::ports::CatalogPage<domain::Track>> parse_artist_tracks(
    std::string_view body, std::uint32_t page, std::uint32_t limit);
domain::Result<application::ports::CatalogPage<domain::Album>> parse_artist_albums(
    std::string_view body, std::uint32_t page, std::uint32_t limit);
domain::Result<domain::Track> parse_track_info(std::string_view body, std::string_view id);

} // namespace music_service::providers::kuwo::parsing
