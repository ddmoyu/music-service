include(Catch)

# Pinned-dependency checks; business tests are added under unit/, integration/ and contract/.
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
