#pragma once

// Helpers shared by the catalog service's translation units; not part of its interface.

#include "application/ports/storage/secret_store.hpp"
#include "domain/catalog/collections.hpp"
#include "domain/catalog/track.hpp"
#include "domain/result.hpp"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace music_service::application::catalog::support {

// Unambiguous cache and flight keys: length-prefixed parts.
inline std::string key_of(std::initializer_list<std::string_view> parts) {
    std::string key;
    for (const auto part : parts) {
        key += std::to_string(part.size());
        key.push_back(':');
        key.append(part);
        key.push_back(';');
    }
    return key;
}

// Only sanitized errors leave a source; internal details stay internal.
inline domain::Error public_error(domain::Error error) {
    if (error.code == domain::ErrorCode::internal) {
        return domain::Error{domain::ErrorCode::internal, "internal error", false, {}};
    }
    return error;
}

inline std::shared_ptr<const ports::SecretBytes> share_secret(std::optional<ports::SecretBytes>& secret) {
    if (!secret) {
        return nullptr;
    }
    return std::make_shared<const ports::SecretBytes>(std::move(*secret));
}

// Rough cached sizes; the caches bound bytes, not items.
inline std::size_t item_bytes(const domain::Track& track) {
    std::size_t bytes = 256 + track.title.size() + track.ref.id.value().size() + track.artwork.value_or("").size();
    for (const auto& artist : track.artists) {
        bytes += 64 + artist.name.size();
    }
    if (track.album) {
        bytes += 64 + track.album->title.size();
    }
    return bytes;
}
inline std::size_t item_bytes(const domain::Playlist& p) {
    return 256 + p.name.size() + p.ref.id.value().size() + p.creator.value_or("").size() +
           p.description.value_or("").size() + p.artwork.value_or("").size();
}
inline std::size_t item_bytes(const domain::Album& a) {
    std::size_t bytes = 256 + a.title.size() + a.ref.id.value().size() + a.artwork.value_or("").size() +
                        a.description.value_or("").size();
    for (const auto& artist : a.artists) {
        bytes += 64 + artist.name.size();
    }
    return bytes;
}
inline std::size_t item_bytes(const domain::Artist& a) {
    return 192 + a.name.size() + a.ref.id.value().size() + a.artwork.value_or("").size();
}
inline std::size_t item_bytes(const domain::Category& c) {
    return 128 + c.id.size() + c.name.size() + c.group.value_or("").size();
}
inline std::size_t item_bytes(const domain::Chart& c) {
    return 256 + c.name.size() + c.ref.id.value().size() + c.description.value_or("").size();
}
inline std::size_t item_bytes(const domain::TrackLyrics& l) {
    std::size_t bytes = 128 + l.lrc.value_or("").size() + l.translated_lrc.value_or("").size();
    for (const auto& line : l.karaoke) {
        bytes += 32;
        for (const auto& word : line.words) {
            bytes += 32 + word.text.size();
        }
    }
    return bytes;
}

} // namespace music_service::application::catalog::support
