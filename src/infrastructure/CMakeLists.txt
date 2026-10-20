# Adapters for configuration, observability and (later) transport, storage, files and secrets.
# transport uses cpp-httplib with Mbed TLS, storage uses SQLite, config uses JSON.
add_library(music_service_infrastructure STATIC
    config/address.cpp
    config/config_loader.cpp
    config/config_validate.cpp
    observability/correlation.cpp
    observability/logger.cpp
    observability/redaction.cpp
)
add_library(music_service::infrastructure ALIAS music_service_infrastructure)
target_link_libraries(music_service_infrastructure
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        music_service::third_party_httplib
        music_service::third_party_sqlite
        nlohmann_json::nlohmann_json
)
