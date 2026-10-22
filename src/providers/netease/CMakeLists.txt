# NetEase Cloud Music protocol package. Links only what the platform itself needs: JSON
# parsing and Mbed TLS crypto for the web client's request encryption (weapi).
add_library(music_service_provider_netease STATIC
    bundle.cpp
    capabilities.cpp
    parsing/parsers.cpp
    protocol/requests.cpp
    protocol/weapi.cpp
)
add_library(music_service::provider_netease ALIAS music_service_provider_netease)
target_link_libraries(music_service_provider_netease
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        nlohmann_json::nlohmann_json
        mbedcrypto
)
