add_library(music_service_provider_kugou STATIC
    bundle.cpp client.cpp parsing.cpp catalog.cpp audio.cpp artists.cpp web_audio.cpp
)
add_library(music_service::provider_kugou ALIAS music_service_provider_kugou)
target_link_libraries(music_service_provider_kugou
    PUBLIC music_service::application
    PRIVATE music_service::project_warnings nlohmann_json::nlohmann_json mbedcrypto
)
