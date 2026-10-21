#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace music_service::domain {

// What the leading bytes of a resource look like. Only the bytes count: a URL suffix,
// a declared format or a Content-Type is never evidence of the container.
enum class ContentKind {
    audio,   // a recognized audio container
    markup,  // HTML or XML, typically an upstream error or login page
    json,    // a JSON body, typically an upstream error
    text,    // other printable text
    unknown, // binary that matches no recognized container
    empty,
};

enum class Container { mp3, aac_adts, flac, ogg, mp4, wav };

struct ContentSniff {
    ContentKind kind{ContentKind::unknown};
    std::optional<Container> container; // set only for ContentKind::audio
};

// Bytes worth passing to sniff_content; fewer still work, with less certainty.
inline constexpr std::size_t sniff_bytes = 512;

ContentSniff sniff_content(std::string_view head) noexcept;

// "mp3", "aac", "flac", "ogg", "m4a", "wav": the names providers declare and the API reports.
std::string_view to_string(Container value) noexcept;
std::string_view to_string(ContentKind value) noexcept;

} // namespace music_service::domain
