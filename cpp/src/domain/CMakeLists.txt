# Value types and pure rules. No I/O and no third-party dependencies.
add_library(music_dl_domain STATIC
    error.cpp
    assets/quality.cpp
    catalog/refs.cpp
    identity/account.cpp
    tasks/download_intent.cpp
    tasks/task_status.cpp
)
add_library(music_dl::domain ALIAS music_dl_domain)
target_include_directories(music_dl_domain
    PUBLIC "$<BUILD_INTERFACE:${music_dl_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(music_dl_domain
    PUBLIC music_dl::project_options
    PRIVATE music_dl::project_warnings
)
