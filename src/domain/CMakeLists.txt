# Value types and pure rules. No I/O and no third-party dependencies.
add_library(music_service_domain STATIC
    error.cpp
    assets/container.cpp
    assets/quality.cpp
    catalog/refs.cpp
    identity/account.cpp
    tasks/download_intent.cpp
    tasks/task_status.cpp
)
add_library(music_service::domain ALIAS music_service_domain)
# All modules include project headers relative to src/, e.g. "domain/result.hpp".
target_include_directories(music_service_domain PUBLIC "$<BUILD_INTERFACE:${music_service_SOURCE_DIR}/src>")
target_link_libraries(music_service_domain
    PUBLIC music_service::project_options
    PRIVATE music_service::project_warnings
)
