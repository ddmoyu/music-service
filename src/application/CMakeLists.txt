# Use cases (accounts, catalog, download tasks, task events, upstream transport rules and hop admission) and the outbound
# ports in ports/, built on domain types.
add_library(music_service_application STATIC
    accounts/credential_service.cpp
    accounts/identity_admin.cpp
    catalog/asset_service.cpp
    catalog/catalog_entities.cpp
    catalog/catalog_service.cpp
    events/publishing_publication_repository.cpp
    events/publishing_task_repository.cpp
    events/task_event_hub.cpp
    media/file_access.cpp
    media/file_bundle.cpp
    media/media_access.cpp
    media/media_tickets.cpp
    tasks/download_service.cpp
    transport/http_rules.cpp
    transport/upstream_client.cpp
    transport/url.cpp
)
add_library(music_service::application ALIAS music_service_application)
target_link_libraries(music_service_application
    PUBLIC
        music_service::domain
    PRIVATE
        music_service::project_warnings
        nlohmann_json::nlohmann_json
)
