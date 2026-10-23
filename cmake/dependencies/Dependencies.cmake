include_guard(GLOBAL)

# All third-party sources are declared here and fetched into the build tree.
# Research clones under third_party/ are never used by the build.

include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/Versions.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/VerifyDependency.cmake")

# A FetchContent subdirectory name that does not exist: the source is populated
# but its own CMakeLists.txt is not added. Used for header-only or adapted sources.
set(MUSIC_SERVICE_NO_UPSTREAM_CMAKE "music-service-adapter-only")

# Mbed TLS: build only the static libraries.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)

# nlohmann/json: no tests or install rules.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

FetchContent_Declare(music_service_mbedtls
    GIT_REPOSITORY "${MUSIC_SERVICE_MBEDTLS_REPOSITORY}"
    GIT_TAG "${MUSIC_SERVICE_MBEDTLS_TAG}"
    GIT_SHALLOW TRUE
    GIT_SUBMODULES "framework"
    GIT_PROGRESS TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(music_service_httplib
    GIT_REPOSITORY "${MUSIC_SERVICE_HTTPLIB_REPOSITORY}"
    GIT_TAG "${MUSIC_SERVICE_HTTPLIB_TAG}"
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
    SOURCE_SUBDIR "${MUSIC_SERVICE_NO_UPSTREAM_CMAKE}"
)
FetchContent_Declare(music_service_json
    GIT_REPOSITORY "${MUSIC_SERVICE_JSON_REPOSITORY}"
    GIT_TAG "${MUSIC_SERVICE_JSON_TAG}"
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
    EXCLUDE_FROM_ALL
)
# TagLib: a static library with the formats the processors use, no zlib, bindings,
# examples or tests, and utfcpp only from its pinned submodule (never a system copy).
set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(BUILD_BINDINGS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
foreach(format APE ASF DSF MATROSKA MOD SHORTEN TRUEAUDIO)
    set(WITH_${format} OFF CACHE BOOL "" FORCE)
endforeach()
set(WITH_MP4 ON CACHE BOOL "" FORCE)
set(WITH_RIFF ON CACHE BOOL "" FORCE)
set(WITH_VORBIS ON CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_utf8cpp ON)
set(_music_service_taglib_source "${FETCHCONTENT_BASE_DIR}/music_service_taglib-src")
if(FETCHCONTENT_SOURCE_DIR_MUSIC_SERVICE_TAGLIB)
    set(_music_service_taglib_source "${FETCHCONTENT_SOURCE_DIR_MUSIC_SERVICE_TAGLIB}")
endif()
set(utf8cpp_INCLUDE_DIR "${_music_service_taglib_source}/3rdparty/utfcpp/source"
    CACHE PATH "utfcpp headers of the pinned TagLib submodule" FORCE)
FetchContent_Declare(music_service_taglib
    GIT_REPOSITORY "${MUSIC_SERVICE_TAGLIB_REPOSITORY}"
    GIT_TAG "${MUSIC_SERVICE_TAGLIB_TAG}"
    GIT_SHALLOW TRUE
    GIT_SUBMODULES "3rdparty/utfcpp"
    GIT_PROGRESS TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(music_service_sqlite
    URL "${MUSIC_SERVICE_SQLITE_URL}"
    URL_HASH "SHA3_256=${MUSIC_SERVICE_SQLITE_SHA3_256}"
    # Extraction time, not the archive time: an upgraded source must look newer than
    # objects built from the previous version, or Ninja would keep the old objects.
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR "${MUSIC_SERVICE_NO_UPSTREAM_CMAKE}"
)

FetchContent_MakeAvailable(music_service_mbedtls music_service_httplib music_service_json music_service_sqlite)
# TagLib reads BUILD_TESTING for its own CppUnit tests; they are never built here.
set(music_service_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
FetchContent_MakeAvailable(music_service_taglib)
set(BUILD_TESTING "${music_service_build_testing}")

set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(music_service_zlib
    URL "${MUSIC_SERVICE_ZLIB_URL}"
    URL_HASH "SHA256=${MUSIC_SERVICE_ZLIB_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(music_service_zlib)
music_service_verify_file_hashes(zlib "${music_service_zlib_SOURCE_DIR}" ${MUSIC_SERVICE_ZLIB_FILE_SHA3_256})

music_service_verify_git_dependency(mbedtls "${music_service_mbedtls_SOURCE_DIR}" "${MUSIC_SERVICE_MBEDTLS_COMMIT}")
music_service_verify_git_dependency(cpp-httplib "${music_service_httplib_SOURCE_DIR}" "${MUSIC_SERVICE_HTTPLIB_COMMIT}")
music_service_verify_git_dependency(nlohmann-json "${music_service_json_SOURCE_DIR}" "${MUSIC_SERVICE_JSON_COMMIT}")
music_service_verify_git_dependency(taglib "${music_service_taglib_SOURCE_DIR}" "${MUSIC_SERVICE_TAGLIB_COMMIT}")
# The archive hash only covers downloads; the file hashes also cover source overrides.
music_service_verify_file_hashes("sqlite ${MUSIC_SERVICE_SQLITE_VERSION}"
    "${music_service_sqlite_SOURCE_DIR}" ${MUSIC_SERVICE_SQLITE_FILE_SHA3_256})

include("${PROJECT_SOURCE_DIR}/cmake/adapters/httplib.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/adapters/sqlite.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/adapters/taglib.cmake")

if(BUILD_TESTING)
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(music_service_catch2
        GIT_REPOSITORY "${MUSIC_SERVICE_CATCH2_REPOSITORY}"
        GIT_TAG "${MUSIC_SERVICE_CATCH2_TAG}"
        GIT_SHALLOW TRUE
        GIT_SUBMODULES ""
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(music_service_catch2)
    music_service_verify_git_dependency(catch2 "${music_service_catch2_SOURCE_DIR}" "${MUSIC_SERVICE_CATCH2_COMMIT}")
    list(APPEND CMAKE_MODULE_PATH "${music_service_catch2_SOURCE_DIR}/extras")
endif()
