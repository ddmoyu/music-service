# Adapters for configuration, observability and (later) transport, storage, files and secrets.
# transport uses cpp-httplib with Mbed TLS, storage uses SQLite, config uses JSON.
add_library(music_dl_infrastructure STATIC
    config/address.cpp
    config/config_loader.cpp
    config/config_validate.cpp
    observability/correlation.cpp
    observability/logger.cpp
    observability/redaction.cpp
)
add_library(music_dl::infrastructure ALIAS music_dl_infrastructure)
target_link_libraries(music_dl_infrastructure
    PUBLIC
        music_dl::application
    PRIVATE
        music_dl::project_warnings
        music_dl::third_party_httplib
        music_dl::third_party_sqlite
        nlohmann_json::nlohmann_json
)
