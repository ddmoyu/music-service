# No implementation sources yet; this target records module dependencies.
# The media HTTP entry adapts cpp-httplib and JSON at the protocol boundary.
add_library(music_service_media_server INTERFACE)
add_library(music_service::media_server ALIAS music_service_media_server)
target_link_libraries(music_service_media_server INTERFACE
    music_service::application
    music_service::third_party_httplib
    nlohmann_json::nlohmann_json
)
