include(Catch)

# Pinned-dependency checks.
add_executable(music_service_dependency_tests dependencies/third_party_smoke_test.cpp)
target_link_libraries(music_service_dependency_tests PRIVATE
    music_service::project_options
    music_service::project_warnings
    music_service::third_party_httplib
    music_service::third_party_sqlite
    music_service::third_party_taglib
    nlohmann_json::nlohmann_json
    Catch2::Catch2WithMain
)

string(REGEX REPLACE "^v" "" taglib_version "${MUSIC_SERVICE_TAGLIB_TAG}")
string(REGEX REPLACE "^v" "" httplib_version "${MUSIC_SERVICE_HTTPLIB_TAG}")
string(REGEX REPLACE "^v" "" json_version "${MUSIC_SERVICE_JSON_TAG}")
string(REGEX REPLACE "^v" "" mbedtls_version "${MUSIC_SERVICE_MBEDTLS_TAG}")
target_compile_definitions(music_service_dependency_tests PRIVATE
    MUSIC_SERVICE_EXPECTED_HTTPLIB_VERSION="${httplib_version}"
    MUSIC_SERVICE_EXPECTED_JSON_VERSION="${json_version}"
    MUSIC_SERVICE_EXPECTED_MBEDTLS_VERSION="${mbedtls_version}"
    MUSIC_SERVICE_EXPECTED_SQLITE_VERSION="${MUSIC_SERVICE_SQLITE_VERSION}"
    MUSIC_SERVICE_EXPECTED_TAGLIB_VERSION="${taglib_version}"
)
catch_discover_tests(music_service_dependency_tests DISCOVERY_MODE PRE_TEST)

# Domain rules, application ports, the provider registry, platform protocol packages
# against recorded fixtures and infrastructure basics. Uses temporary directories but
# no network or databases (opt-in [online-*] cases aside).
add_executable(music_service_unit_tests
    unit/audio_processing/tagging_test.cpp
    unit/audio_processing/verifying_processor_test.cpp
    unit/downloads/resume_policy_test.cpp
    unit/domain/account_test.cpp
    unit/domain/container_test.cpp
    unit/domain/download_intent_test.cpp
    unit/domain/quality_test.cpp
    unit/domain/refs_test.cpp
    unit/domain/result_test.cpp
    unit/domain/task_status_test.cpp
    unit/application/media_tickets_test.cpp
    unit/application/ports_test.cpp
    unit/application/task_events_test.cpp
    unit/application/transport_rules_test.cpp
    unit/infrastructure/config_test.cpp
    unit/infrastructure/filesystem_test.cpp
    unit/infrastructure/observability_test.cpp
    unit/infrastructure/secret_store_test.cpp
    unit/providers/kuwo_collections_test.cpp
    unit/providers/kuwo_test.cpp
    unit/providers/registry_test.cpp
    unit/runtime/admission_ledger_test.cpp
    unit/runtime/executor_test.cpp
)
# Platform packages left out of the build have no tests to run.
if("fivesing" IN_LIST MUSIC_SERVICE_PROVIDERS)
    target_sources(music_service_unit_tests PRIVATE unit/providers/fivesing_test.cpp)
endif()
target_link_libraries(music_service_unit_tests PRIVATE
    music_service::application
    music_service::audio_processing
    music_service::downloads
    music_service::infrastructure
    music_service::providers
    music_service::runtime
    music_service::third_party_taglib
    nlohmann_json::nlohmann_json
    music_service::project_warnings
    Catch2::Catch2WithMain
)
target_compile_definitions(music_service_unit_tests PRIVATE
    MUSIC_SERVICE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
if(TARGET music_service::external)
    target_sources(music_service_unit_tests PRIVATE unit/providers/helper_protocol_test.cpp)
    target_link_libraries(music_service_unit_tests PRIVATE music_service::external)
endif()
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

# Infrastructure and use cases against real local resources: SQLite and secret files,
# and HTTP(S) servers on 127.0.0.1 with the test certificates. No outside network.
add_executable(music_service_integration_tests
    integration/accounts_test.cpp
    integration/backup_test.cpp
    integration/catalog_test.cpp
    integration/collections_test.cpp
    integration/downloads_test.cpp
    integration/events_test.cpp
    integration/kuwo_gate_test.cpp
    integration/media_proxy_test.cpp
    integration/media_test.cpp
    integration/publication_test.cpp
    integration/runner_test.cpp
    integration/s3_gate_test.cpp
    integration/server_test.cpp
    integration/storage_test.cpp
    integration/transport_test.cpp
)
if("fivesing" IN_LIST MUSIC_SERVICE_PROVIDERS)
    target_sources(music_service_integration_tests PRIVATE integration/fivesing_gate_test.cpp
                                                           integration/s2_gate_test.cpp)
endif()
target_link_libraries(music_service_integration_tests PRIVATE
    music_service::application
    music_service::audio_processing
    music_service::downloads
    music_service::infrastructure
    music_service::providers
    music_service::runtime
    music_service::server_core
    music_service::third_party_httplib
    nlohmann_json::nlohmann_json
    music_service::project_warnings
    Catch2::Catch2WithMain
)
target_compile_definitions(music_service_integration_tests PRIVATE
    MUSIC_SERVICE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
# External helpers (T26): the runner against a stand-in helper program.
if(TARGET music_service::external)
    add_executable(music_service_fake_helper support/fake_helper.cpp)
    target_link_libraries(music_service_fake_helper PRIVATE
        music_service::project_options
        music_service::project_warnings
    )
    target_sources(music_service_integration_tests PRIVATE integration/external_helper_test.cpp)
    target_link_libraries(music_service_integration_tests PRIVATE music_service::external)
    add_dependencies(music_service_integration_tests music_service_fake_helper)
    target_compile_definitions(music_service_integration_tests PRIVATE
        MUSIC_SERVICE_FAKE_HELPER="$<TARGET_FILE:music_service_fake_helper>"
    )
endif()
catch_discover_tests(music_service_integration_tests DISCOVERY_MODE PRE_TEST)
