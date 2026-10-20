# No implementation sources yet; this target records module dependencies.
add_library(music_dl_providers INTERFACE)
add_library(music_dl::providers ALIAS music_dl_providers)
target_link_libraries(music_dl_providers INTERFACE music_dl::application)
