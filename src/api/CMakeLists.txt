# No implementation sources yet; this target records module dependencies.
# The control HTTP entry adapts cpp-httplib and JSON at the protocol boundary.
add_library(music_dl_http_api INTERFACE)
add_library(music_dl::http_api ALIAS music_dl_http_api)
target_link_libraries(music_dl_http_api INTERFACE
    music_dl::application
    music_dl::third_party_httplib
    nlohmann_json::nlohmann_json
)
