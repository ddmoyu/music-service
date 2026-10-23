include_guard(GLOBAL)

# TagLib's own target only exports include paths for an installed tree. This target
# adds the source-tree header directories (as system headers, so project warnings do
# not apply to them) and the generated taglib_config.h.
set(music_service_taglib_root "${music_service_taglib_SOURCE_DIR}/taglib")
add_library(music_service_third_party_taglib INTERFACE)
add_library(music_service::third_party_taglib ALIAS music_service_third_party_taglib)
target_link_libraries(music_service_third_party_taglib INTERFACE tag)
target_include_directories(music_service_third_party_taglib SYSTEM INTERFACE
    "${music_service_taglib_root}"
    "${music_service_taglib_root}/toolkit"
    "${music_service_taglib_root}/mpeg"
    "${music_service_taglib_root}/mpeg/id3v2"
    "${music_service_taglib_root}/mpeg/id3v2/frames"
    "${music_service_taglib_root}/mpeg/id3v1"
    "${music_service_taglib_root}/ogg"
    "${music_service_taglib_root}/ogg/flac"
    "${music_service_taglib_root}/flac"
    "${music_service_taglib_root}/ogg/vorbis"
    "${music_service_taglib_root}/mp4"
    "${music_service_taglib_root}/riff"
    "${music_service_taglib_root}/riff/wav"
    "${music_service_taglib_BINARY_DIR}"
)
# Its sources are UTF-8; without this MSVC reads them in the build machine's code page.
if(MSVC)
    target_compile_options(tag PRIVATE /utf-8)
endif()
