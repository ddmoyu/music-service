include_guard(GLOBAL)

# Release package (T37): one archive per system and architecture, laid out as
#   bin/music-service[.exe]
#   share/music-service/README.md              installing and running
#   share/music-service/config.example.json
#   share/music-service/licenses/              the project's dependencies, as shipped
#   share/music-service/service/               Windows service or systemd integration
# Build it with the release preset, then: cmake --build --preset <preset> --target package
# The program links its runtimes statically (MSVC runtime; libstdc++/libgcc on Linux), so
# the archive holds no other libraries. Data never lives in the installation directory.

set(_share share/music-service)

install(FILES
    "${PROJECT_SOURCE_DIR}/packaging/package/README.md"
    "${PROJECT_SOURCE_DIR}/packaging/package/config.example.json"
    DESTINATION ${_share})

# Licenses of what the program contains: the project's own dependencies only (Catch2 is
# a test dependency and not shipped).
install(FILES "${music_service_mbedtls_SOURCE_DIR}/LICENSE" DESTINATION ${_share}/licenses RENAME mbedtls-LICENSE.txt)
install(FILES "${music_service_httplib_SOURCE_DIR}/LICENSE" DESTINATION ${_share}/licenses RENAME cpp-httplib-LICENSE.txt)
install(FILES "${music_service_json_SOURCE_DIR}/LICENSE.MIT" DESTINATION ${_share}/licenses RENAME nlohmann-json-LICENSE.txt)
install(FILES "${music_service_zlib_SOURCE_DIR}/LICENSE" DESTINATION ${_share}/licenses RENAME zlib-LICENSE.txt)
install(FILES "${music_service_taglib_SOURCE_DIR}/COPYING.MPL" DESTINATION ${_share}/licenses RENAME taglib-COPYING.MPL.txt)
install(FILES "${music_service_taglib_SOURCE_DIR}/3rdparty/utfcpp/LICENSE" DESTINATION ${_share}/licenses
        RENAME utfcpp-LICENSE.txt)
install(FILES "${PROJECT_SOURCE_DIR}/packaging/package/THIRD-PARTY.md" DESTINATION ${_share}/licenses)
install(FILES "${PROJECT_SOURCE_DIR}/packaging/package/licenses/unlock-music-LICENSE.txt"
        DESTINATION ${_share}/licenses)

if(WIN32)
    install(FILES
        "${PROJECT_SOURCE_DIR}/packaging/windows/install-service.ps1"
        "${PROJECT_SOURCE_DIR}/packaging/windows/uninstall-service.ps1"
        DESTINATION ${_share}/service)
else()
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/music-service.service" DESTINATION ${_share}/service)
    install(PROGRAMS "${PROJECT_SOURCE_DIR}/packaging/linux/install.sh" DESTINATION ${_share}/service)
endif()

# music-service-<version>-<system>-<arch>.zip / .tar.gz
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _arch)
if(_arch MATCHES "^(amd64|x86_64|x64)$")
    set(_arch x86_64)
elseif(_arch MATCHES "^(arm64|aarch64)$")
    set(_arch arm64)
endif()
string(TOLOWER "${CMAKE_SYSTEM_NAME}" _system)

set(CPACK_PACKAGE_NAME music-service)
set(CPACK_PACKAGE_VENDOR music-service)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_FILE_NAME "music-service-${PROJECT_VERSION}-${_system}-${_arch}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_STRIP_FILES ON)
if(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
include(CPack)
