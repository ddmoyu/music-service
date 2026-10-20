# No implementation sources yet; this target records module dependencies.
# The control HTTP entry adapts cpp-httplib and JSON at the protocol boundary.
add_library(music_service_http_api INTERFACE)
add_library(music_service::http_api ALIAS music_service_http_api)
target_link_libraries(music_service_http_api INTERFACE
    music_service::application
    music_service::third_party_httplib
    nlohmann_json::nlohmann_json
)
