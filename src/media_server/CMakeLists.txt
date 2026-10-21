# The media entry (files and tickets; the online proxy follows with T34) adapts
# cpp-httplib and JSON at the protocol boundary.
add_library(music_service_media_server STATIC
    httplib/media_server.cpp
)
add_library(music_service::media_server ALIAS music_service_media_server)
target_link_libraries(music_service_media_server
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        music_service::third_party_httplib
        nlohmann_json::nlohmann_json
)
