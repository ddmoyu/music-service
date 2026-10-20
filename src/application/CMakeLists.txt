# Use cases come later; for now application provides header-only outbound ports
# (src/application/ports) built on domain types.
add_library(music_service_application INTERFACE)
add_library(music_service::application ALIAS music_service_application)
target_link_libraries(music_service_application INTERFACE music_service::domain)
