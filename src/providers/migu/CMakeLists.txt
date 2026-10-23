add_library(music_service_provider_migu STATIC
    bundle.cpp client.cpp parsing.cpp catalog.cpp audio.cpp
)
add_library(music_service::provider_migu ALIAS music_service_provider_migu)
target_link_libraries(music_service_provider_migu
    PUBLIC music_service::application
    PRIVATE music_service::project_warnings nlohmann_json::nlohmann_json mbedcrypto
)
