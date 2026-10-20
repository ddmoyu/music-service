# No implementation sources yet; this target records module dependencies.
add_library(music_dl_downloads INTERFACE)
add_library(music_dl::downloads ALIAS music_dl_downloads)
target_link_libraries(music_dl_downloads INTERFACE music_dl::application)
