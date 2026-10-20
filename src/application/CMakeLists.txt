# Use cases come later; for now application provides header-only outbound ports
# (src/application/ports) built on domain types.
add_library(music_dl_application INTERFACE)
add_library(music_dl::application ALIAS music_dl_application)
target_link_libraries(music_dl_application INTERFACE music_dl::domain)
