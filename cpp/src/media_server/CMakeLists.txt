# No implementation sources yet; this target records module dependencies.
add_library(music_dl_media_server INTERFACE)
add_library(music_dl::media_server ALIAS music_dl_media_server)
target_link_libraries(music_dl_media_server INTERFACE music_dl::application)
