# No implementation sources yet; this target records module dependencies.
add_library(music_dl_application INTERFACE)
add_library(music_dl::application ALIAS music_dl_application)
target_link_libraries(music_dl_application INTERFACE music_dl::domain)
