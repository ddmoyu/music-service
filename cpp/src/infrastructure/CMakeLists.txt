# No implementation sources yet; this target records module dependencies.
add_library(music_dl_infrastructure INTERFACE)
add_library(music_dl::infrastructure ALIAS music_dl_infrastructure)
target_link_libraries(music_dl_infrastructure INTERFACE music_dl::application)
