# Adapters for access tokens, configuration, observability, files, secrets and SQLite
# storage and upstream HTTP(S). Access tokens use Mbed TLS for randomness and SHA-256,
# transport uses cpp-httplib with Mbed TLS, storage uses SQLite, config uses JSON.
add_library(music_service_infrastructure STATIC
    auth/access_tokens.cpp
    config/address.cpp
    config/config_loader.cpp
    config/config_validate.cpp
    filesystem/data_layout.cpp
    filesystem/destination_files.cpp
    filesystem/files.cpp
    filesystem/instance_lock.cpp
    filesystem/paths.cpp
    observability/correlation.cpp
    observability/logger.cpp
    observability/redaction.cpp
    secrets/file_secret_store.cpp
    storage/intent_codec.cpp
    storage/sqlite/account_repositories.cpp
    storage/sqlite/connection.cpp
    storage/sqlite/database.cpp
    storage/sqlite/migrations.cpp
    storage/sqlite/task_repositories.cpp
    transport/httplib_transport.cpp
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
if(WIN32)
    # Owner-only ACLs for secrets and private directories.
    target_link_libraries(music_service_infrastructure PRIVATE advapi32)
endif()
