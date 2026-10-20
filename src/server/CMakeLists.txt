# Executable entry: command line, offline identity commands, ProcessHost lifecycle and
# dependency composition. The core library holds everything but main() for tests.
add_library(music_service_server_core STATIC
    commands.cpp
    instance.cpp
    process_host.cpp
)
add_library(music_service::server_core ALIAS music_service_server_core)
target_compile_definitions(music_service_server_core PUBLIC
    MUSIC_SERVICE_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(music_service_server_core
    PUBLIC
        music_service::application
        music_service::infrastructure
    PRIVATE
        music_service::project_warnings
        music_service::http_api
        music_service::providers
        music_service::runtime
        music_service::third_party_httplib
        nlohmann_json::nlohmann_json
)

add_executable(music_service_server main.cpp)
set_target_properties(music_service_server PROPERTIES OUTPUT_NAME music-service)
target_link_libraries(music_service_server PRIVATE
    music_service::server_core
    music_service::project_warnings
)
