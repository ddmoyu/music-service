# The control HTTP entry adapts cpp-httplib and JSON at the protocol boundary.
add_library(music_service_http_api STATIC
    httplib/control_api.cpp
)
add_library(music_service::http_api ALIAS music_service_http_api)
target_link_libraries(music_service_http_api
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        music_service::third_party_httplib
        nlohmann_json::nlohmann_json
)
