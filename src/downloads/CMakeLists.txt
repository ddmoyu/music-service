# No implementation sources yet; this target records module dependencies.
add_library(music_service_downloads INTERFACE)
add_library(music_service::downloads ALIAS music_service_downloads)
target_link_libraries(music_service_downloads INTERFACE music_service::application)
