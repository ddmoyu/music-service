include_guard(GLOBAL)

# cpp-httplib is consumed as a header with an explicit TLS backend instead of
# its own CMakeLists, which would search the system for OpenSSL/zlib/brotli.
# Compression stays disabled until a feature needs it.

find_package(Threads REQUIRED)

add_library(music_service_third_party_httplib INTERFACE)
add_library(music_service::third_party_httplib ALIAS music_service_third_party_httplib)
target_include_directories(music_service_third_party_httplib SYSTEM INTERFACE
    "${music_service_httplib_SOURCE_DIR}"
)
target_compile_definitions(music_service_third_party_httplib INTERFACE
    CPPHTTPLIB_MBEDTLS_SUPPORT
)
target_link_libraries(music_service_third_party_httplib INTERFACE
    mbedtls
    mbedx509
    mbedcrypto
    Threads::Threads
)
if(WIN32)
    target_link_libraries(music_service_third_party_httplib INTERFACE ws2_32 crypt32)
    # httplib includes <windows.h>; keep min/max macros out of project code.
    # cpp-httplib requires Windows 10 or later, which is also the supported baseline.
    target_compile_definitions(music_service_third_party_httplib INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    )
endif()
