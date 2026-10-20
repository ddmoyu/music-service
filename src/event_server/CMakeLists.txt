# No implementation sources yet; this target records module dependencies.
# The SSE entry adapts cpp-httplib and JSON at the protocol boundary.
add_library(music_service_event_server INTERFACE)
add_library(music_service::event_server ALIAS music_service_event_server)
target_link_libraries(music_service_event_server INTERFACE
    music_service::application
    music_service::third_party_httplib
    nlohmann_json::nlohmann_json
)
