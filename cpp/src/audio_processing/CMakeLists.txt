# No implementation sources yet; this target records module dependencies.
add_library(music_dl_audio_processing INTERFACE)
add_library(music_dl::audio_processing ALIAS music_dl_audio_processing)
target_link_libraries(music_dl_audio_processing INTERFACE music_dl::application)
