# No implementation sources yet; this target records module dependencies.
add_library(music_service_audio_processing INTERFACE)
add_library(music_service::audio_processing ALIAS music_service_audio_processing)
target_link_libraries(music_service_audio_processing INTERFACE music_service::application)
