# Kuwo protocol package. Links only what the platform itself needs: JSON parsing.
add_library(music_service_provider_kuwo STATIC
    bundle.cpp
    capabilities.cpp
    client.cpp
    collections.cpp
    parsing/collections.cpp
    parsing/parsers.cpp
    protocol/requests.cpp
)
add_library(music_service::provider_kuwo ALIAS music_service_provider_kuwo)
target_link_libraries(music_service_provider_kuwo
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        nlohmann_json::nlohmann_json
)
