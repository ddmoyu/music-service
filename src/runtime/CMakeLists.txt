# No implementation sources yet; this target records module dependencies.
add_library(music_service_runtime INTERFACE)
add_library(music_service::runtime ALIAS music_service_runtime)
target_link_libraries(music_service_runtime INTERFACE music_service::application)
