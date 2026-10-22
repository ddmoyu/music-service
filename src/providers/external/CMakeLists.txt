# Optional external helpers (T26): the versioned line protocol, the process runner and
# the child-process primitive. Pure C++ and system APIs; no Python, Node or FFmpeg is
# needed to build or run the service, only by the helper programs an operator installs.
add_library(music_service_external STATIC
    child_process.cpp
    helper_protocol.cpp
    helper_runner.cpp
)
add_library(music_service::external ALIAS music_service_external)
find_package(Threads REQUIRED)
target_link_libraries(music_service_external
    PUBLIC
        music_service::application
        music_service::infrastructure
        Threads::Threads
    PRIVATE
        music_service::project_warnings
        nlohmann_json::nlohmann_json
)
