# Kuwo protocol package. Links only what the platform itself needs.
add_library(music_service_provider_kuwo STATIC
    bundle.cpp
)
add_library(music_service::provider_kuwo ALIAS music_service_provider_kuwo)
target_link_libraries(music_service_provider_kuwo
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
)
