# No implementation sources yet; this target records module dependencies.
# transport uses cpp-httplib with Mbed TLS, storage uses SQLite, config uses JSON.
add_library(music_dl_infrastructure INTERFACE)
add_library(music_dl::infrastructure ALIAS music_dl_infrastructure)
target_link_libraries(music_dl_infrastructure INTERFACE
    music_dl::application
    music_dl::third_party_httplib
    music_dl::third_party_sqlite
    nlohmann_json::nlohmann_json
)
