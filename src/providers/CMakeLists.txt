# No implementation sources yet; this target records module dependencies.
add_library(music_service_providers INTERFACE)
add_library(music_service::providers ALIAS music_service_providers)
target_link_libraries(music_service_providers INTERFACE music_service::application)
