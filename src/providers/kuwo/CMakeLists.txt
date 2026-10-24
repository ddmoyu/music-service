# 酷我原生协议：JSON、歌词 base64／zlib，字符转换使用系统能力。
add_library(music_service_provider_kuwo STATIC
    bundle.cpp
    auth.cpp
    artists.cpp
    capabilities.cpp
    client.cpp
    collections.cpp
    parsing/collections.cpp
    parsing/artists.cpp
    parsing/parsers.cpp
    parsing/lyrics.cpp
    protocol/requests.cpp
)
add_library(music_service::provider_kuwo ALIAS music_service_provider_kuwo)
target_link_libraries(music_service_provider_kuwo
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        nlohmann_json::nlohmann_json
        ZLIB::ZLIBSTATIC
        mbedcrypto
)
if(NOT WIN32)
    find_package(Iconv REQUIRED)
    target_link_libraries(music_service_provider_kuwo PRIVATE Iconv::Iconv)
endif()
