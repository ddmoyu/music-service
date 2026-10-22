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
        music_service::audio_processing
        music_service::downloads
        music_service::event_server
        music_service::http_api
        music_service::media_server
        music_service::providers
        music_service::runtime
        music_service::third_party_httplib
        nlohmann_json::nlohmann_json
)

add_executable(music_service_server main.cpp)
if(WIN32)
    # "service": serve under the Windows Service Control Manager.
    target_sources(music_service_server PRIVATE windows_service.cpp)
    target_link_libraries(music_service_server PRIVATE advapi32)
endif()
set_target_properties(music_service_server PROPERTIES OUTPUT_NAME music-service)
target_link_libraries(music_service_server PRIVATE
    music_service::server_core
    music_service::project_warnings
)
option(MUSIC_SERVICE_STATIC_CXX_RUNTIME
       "Link libstdc++ and libgcc statically into the Linux program (only glibc stays shared)" ON)
if(MUSIC_SERVICE_STATIC_CXX_RUNTIME AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(music_service_server PRIVATE -static-libstdc++ -static-libgcc)
endif()
install(TARGETS music_service_server RUNTIME DESTINATION bin)
