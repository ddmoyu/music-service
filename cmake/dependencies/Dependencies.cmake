include_guard(GLOBAL)

# All third-party sources are declared here and fetched into the build tree.
# Research clones under third_party/ are never used by the build.

include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/Versions.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/VerifyDependency.cmake")

# A FetchContent subdirectory name that does not exist: the source is populated
# but its own CMakeLists.txt is not added. Used for header-only or adapted sources.
set(MUSIC_DL_NO_UPSTREAM_CMAKE "music-dl-adapter-only")

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

FetchContent_Declare(music_dl_mbedtls
    GIT_REPOSITORY "${MUSIC_DL_MBEDTLS_REPOSITORY}"
    GIT_TAG "${MUSIC_DL_MBEDTLS_TAG}"
    GIT_SHALLOW TRUE
    GIT_SUBMODULES "framework"
    GIT_PROGRESS TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(music_dl_httplib
    GIT_REPOSITORY "${MUSIC_DL_HTTPLIB_REPOSITORY}"
    GIT_TAG "${MUSIC_DL_HTTPLIB_TAG}"
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
    SOURCE_SUBDIR "${MUSIC_DL_NO_UPSTREAM_CMAKE}"
)
FetchContent_Declare(music_dl_json
    GIT_REPOSITORY "${MUSIC_DL_JSON_REPOSITORY}"
    GIT_TAG "${MUSIC_DL_JSON_TAG}"
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(music_dl_sqlite
    URL "${MUSIC_DL_SQLITE_URL}"
    URL_HASH "SHA3_256=${MUSIC_DL_SQLITE_SHA3_256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR "${MUSIC_DL_NO_UPSTREAM_CMAKE}"
)

FetchContent_MakeAvailable(music_dl_mbedtls music_dl_httplib music_dl_json music_dl_sqlite)

music_dl_verify_git_dependency(mbedtls "${music_dl_mbedtls_SOURCE_DIR}" "${MUSIC_DL_MBEDTLS_COMMIT}")
music_dl_verify_git_dependency(cpp-httplib "${music_dl_httplib_SOURCE_DIR}" "${MUSIC_DL_HTTPLIB_COMMIT}")
music_dl_verify_git_dependency(nlohmann-json "${music_dl_json_SOURCE_DIR}" "${MUSIC_DL_JSON_COMMIT}")
message(STATUS "Dependency sqlite: ${MUSIC_DL_SQLITE_VERSION}, archive hash verified by FetchContent")

include("${PROJECT_SOURCE_DIR}/cmake/adapters/httplib.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/adapters/sqlite.cmake")

if(BUILD_TESTING)
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(music_dl_catch2
        GIT_REPOSITORY "${MUSIC_DL_CATCH2_REPOSITORY}"
        GIT_TAG "${MUSIC_DL_CATCH2_TAG}"
        GIT_SHALLOW TRUE
        GIT_SUBMODULES ""
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(music_dl_catch2)
    music_dl_verify_git_dependency(catch2 "${music_dl_catch2_SOURCE_DIR}" "${MUSIC_DL_CATCH2_COMMIT}")
    list(APPEND CMAKE_MODULE_PATH "${music_dl_catch2_SOURCE_DIR}/extras")
endif()
