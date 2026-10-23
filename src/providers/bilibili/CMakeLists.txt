# Bilibili 原生视频目录与独立 AAC 音轨，不调用外部下载或转码工具。
add_library(music_service_provider_bilibili STATIC
    bundle.cpp
    protocol.cpp
    catalog.cpp
    audio.cpp
)
add_library(music_service::provider_bilibili ALIAS music_service_provider_bilibili)
target_link_libraries(music_service_provider_bilibili
    PUBLIC music_service::application
    PRIVATE music_service::project_warnings nlohmann_json::nlohmann_json
)
