# Use cases (accounts so far) and the outbound ports in ports/, built on domain types.
add_library(music_service_application STATIC
    accounts/credential_service.cpp
    accounts/identity_admin.cpp
)
add_library(music_service::application ALIAS music_service_application)
target_link_libraries(music_service_application
    PUBLIC
        music_service::domain
    PRIVATE
        music_service::project_warnings
)
