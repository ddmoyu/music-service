# Jamendo 网站协议；摘要使用项目已有的 Mbed TLS。
add_library(music_service_provider_jamendo STATIC
    bundle.cpp client.cpp parsing.cpp catalog.cpp discovery.cpp audio.cpp
)
add_library(music_service::provider_jamendo ALIAS music_service_provider_jamendo)
target_link_libraries(music_service_provider_jamendo
    PUBLIC music_service::application
    PRIVATE music_service::project_warnings nlohmann_json::nlohmann_json mbedcrypto
)
