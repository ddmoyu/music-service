include(Catch)

# Pinned-dependency checks.
add_executable(music_service_dependency_tests dependencies/third_party_smoke_test.cpp)
target_link_libraries(music_service_dependency_tests PRIVATE
    music_service::project_options
    music_service::project_warnings
    music_service::third_party_httplib
    music_service::third_party_sqlite
    nlohmann_json::nlohmann_json
    Catch2::Catch2WithMain
)

string(REGEX REPLACE "^v" "" httplib_version "${MUSIC_SERVICE_HTTPLIB_TAG}")
string(REGEX REPLACE "^v" "" json_version "${MUSIC_SERVICE_JSON_TAG}")
string(REGEX REPLACE "^v" "" mbedtls_version "${MUSIC_SERVICE_MBEDTLS_TAG}")
target_compile_definitions(music_service_dependency_tests PRIVATE
    MUSIC_SERVICE_EXPECTED_HTTPLIB_VERSION="${httplib_version}"
    MUSIC_SERVICE_EXPECTED_JSON_VERSION="${json_version}"
    MUSIC_SERVICE_EXPECTED_MBEDTLS_VERSION="${mbedtls_version}"
    MUSIC_SERVICE_EXPECTED_SQLITE_VERSION="${MUSIC_SERVICE_SQLITE_VERSION}"
)
catch_discover_tests(music_service_dependency_tests DISCOVERY_MODE PRE_TEST)

# Domain rules, application ports, the provider registry and infrastructure basics.
# Uses temporary directories but no network or databases.
add_executable(music_service_unit_tests
    unit/domain/account_test.cpp
    unit/domain/download_intent_test.cpp
    unit/domain/quality_test.cpp
    unit/domain/refs_test.cpp
    unit/domain/result_test.cpp
    unit/domain/task_status_test.cpp
    unit/application/ports_test.cpp
    unit/infrastructure/config_test.cpp
    unit/infrastructure/filesystem_test.cpp
    unit/infrastructure/observability_test.cpp
    unit/infrastructure/secret_store_test.cpp
    unit/providers/registry_test.cpp
    unit/runtime/admission_ledger_test.cpp
    unit/runtime/executor_test.cpp
)
target_link_libraries(music_service_unit_tests PRIVATE
    music_service::application
    music_service::infrastructure
    music_service::providers
    music_service::runtime
    nlohmann_json::nlohmann_json
    music_service::project_warnings
    Catch2::Catch2WithMain
)
catch_discover_tests(music_service_unit_tests DISCOVERY_MODE PRE_TEST)

# Fault injection through a replaced global allocator; kept out of the unit tests.
add_executable(music_service_fault_tests
    fault/executor_construction_test.cpp
)
target_link_libraries(music_service_fault_tests PRIVATE
    music_service::runtime
    music_service::project_warnings
    Catch2::Catch2WithMain
)
catch_discover_tests(music_service_fault_tests DISCOVERY_MODE PRE_TEST)

# Infrastructure and use cases against real local resources (SQLite and secret files);
# no network.
add_executable(music_service_integration_tests
    integration/accounts_test.cpp
    integration/storage_test.cpp
)
target_link_libraries(music_service_integration_tests PRIVATE
    music_service::application
    music_service::infrastructure
    music_service::providers
    music_service::project_warnings
    Catch2::Catch2WithMain
)
catch_discover_tests(music_service_integration_tests DISCOVERY_MODE PRE_TEST)
