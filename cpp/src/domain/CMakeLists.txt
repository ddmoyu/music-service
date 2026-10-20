# INTERFACE records the boundary until real source files are introduced.
add_library(music_dl_domain INTERFACE)
add_library(music_dl::domain ALIAS music_dl_domain)
target_link_libraries(music_dl_domain INTERFACE music_dl::project_options)
target_include_directories(music_dl_domain INTERFACE
    "$<BUILD_INTERFACE:${music_dl_SOURCE_DIR}/include>"
)
