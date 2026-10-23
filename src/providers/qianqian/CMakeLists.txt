# 千千网页协议与资源均由服务原生处理；Mbed TLS 提供协议摘要。
add_library(music_service_provider_qianqian STATIC
    bundle.cpp client.cpp compression.cpp parsing.cpp catalog.cpp audio.cpp
)
add_library(music_service::provider_qianqian ALIAS music_service_provider_qianqian)
target_link_libraries(music_service_provider_qianqian
    PUBLIC music_service::application
    PRIVATE music_service::project_warnings nlohmann_json::nlohmann_json mbedcrypto ZLIB::ZLIBSTATIC
)
