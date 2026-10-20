# No implementation sources yet; this target records module dependencies.
# The media HTTP entry adapts cpp-httplib and JSON at the protocol boundary.
add_library(music_dl_media_server INTERFACE)
add_library(music_dl::media_server ALIAS music_dl_media_server)
target_link_libraries(music_dl_media_server INTERFACE
    music_dl::application
    music_dl::third_party_httplib
    nlohmann_json::nlohmann_json
)
