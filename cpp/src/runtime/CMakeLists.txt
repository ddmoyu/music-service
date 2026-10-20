# No implementation sources yet; this target records module dependencies.
add_library(music_dl_runtime INTERFACE)
add_library(music_dl::runtime ALIAS music_dl_runtime)
target_link_libraries(music_dl_runtime INTERFACE music_dl::application)
