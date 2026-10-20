include(Catch)

# Pinned-dependency checks.
add_executable(music_dl_dependency_tests dependencies/third_party_smoke_test.cpp)
target_link_libraries(music_dl_dependency_tests PRIVATE
    music_dl::project_options
    music_dl::project_warnings
    music_dl::third_party_httplib
    music_dl::third_party_sqlite
    nlohmann_json::nlohmann_json
    Catch2::Catch2WithMain
)

string(REGEX REPLACE "^v" "" httplib_version "${MUSIC_DL_HTTPLIB_TAG}")
string(REGEX REPLACE "^v" "" json_version "${MUSIC_DL_JSON_TAG}")
string(REGEX REPLACE "^v" "" mbedtls_version "${MUSIC_DL_MBEDTLS_TAG}")
target_compile_definitions(music_dl_dependency_tests PRIVATE
    MUSIC_DL_EXPECTED_HTTPLIB_VERSION="${httplib_version}"
    MUSIC_DL_EXPECTED_JSON_VERSION="${json_version}"
    MUSIC_DL_EXPECTED_MBEDTLS_VERSION="${mbedtls_version}"
    MUSIC_DL_EXPECTED_SQLITE_VERSION="${MUSIC_DL_SQLITE_VERSION}"
)
catch_discover_tests(music_dl_dependency_tests DISCOVERY_MODE PRE_TEST)

# Domain rules and application ports; no network, files or databases.
add_executable(music_dl_unit_tests
    unit/domain/account_test.cpp
    unit/domain/download_intent_test.cpp
    unit/domain/quality_test.cpp
    unit/domain/refs_test.cpp
    unit/domain/result_test.cpp
    unit/domain/task_status_test.cpp
    unit/application/ports_test.cpp
)
target_link_libraries(music_dl_unit_tests PRIVATE
    music_dl::application
    music_dl::project_warnings
    Catch2::Catch2WithMain
)
catch_discover_tests(music_dl_unit_tests DISCOVERY_MODE PRE_TEST)
