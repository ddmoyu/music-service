# The SSE entry adapts cpp-httplib and JSON at the protocol boundary.
add_library(music_service_event_server STATIC
    httplib/event_server.cpp
)
add_library(music_service::event_server ALIAS music_service_event_server)
target_link_libraries(music_service_event_server
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        music_service::third_party_httplib
        nlohmann_json::nlohmann_json
)
