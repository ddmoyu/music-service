# QQ 原生 HTTP／JSON 协议；Mbed TLS 仅用于集合游标摘要。
add_library(music_service_provider_qq STATIC
    bundle.cpp
    client.cpp
    parsing.cpp
    catalog.cpp
    audio.cpp
)
add_library(music_service::provider_qq ALIAS music_service_provider_qq)
target_link_libraries(music_service_provider_qq
    PUBLIC music_service::application
    PRIVATE music_service::project_warnings nlohmann_json::nlohmann_json mbedcrypto
)
