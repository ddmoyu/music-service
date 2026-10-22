# 5sing protocol package. Links only what the platform itself needs: JSON parsing.
add_library(music_service_provider_fivesing STATIC
    bundle.cpp
    capabilities.cpp
    parsing/parsers.cpp
    protocol/requests.cpp
)
add_library(music_service::provider_fivesing ALIAS music_service_provider_fivesing)
target_link_libraries(music_service_provider_fivesing
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        nlohmann_json::nlohmann_json
)
