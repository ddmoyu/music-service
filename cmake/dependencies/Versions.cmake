include_guard(GLOBAL)

# Single source of truth for third-party versions.
# Git dependencies: release tag + expected commit (the peeled tag target).
# Archive dependencies: release URL + published hash.
# Update a tag and its commit together; a mismatch stops the configure step.

# cpp-httplib: HTTP server/client, header only. MIT.
set(MUSIC_DL_HTTPLIB_REPOSITORY "https://github.com/yhirose/cpp-httplib.git")
set(MUSIC_DL_HTTPLIB_TAG "v0.62.0")
set(MUSIC_DL_HTTPLIB_COMMIT "74ef3c42188acd6391f027c4337fb0e1a68c0024")

# nlohmann/json: JSON at protocol boundaries. MIT.
set(MUSIC_DL_JSON_REPOSITORY "https://github.com/nlohmann/json.git")
set(MUSIC_DL_JSON_TAG "v3.12.0")
set(MUSIC_DL_JSON_COMMIT "55f93686c01528224f448c19128836e7df245f72")

# Mbed TLS 3.6 LTS: TLS backend for cpp-httplib. Apache-2.0 OR GPL-2.0-or-later.
set(MUSIC_DL_MBEDTLS_REPOSITORY "https://github.com/Mbed-TLS/mbedtls.git")
set(MUSIC_DL_MBEDTLS_TAG "v3.6.7")
set(MUSIC_DL_MBEDTLS_COMMIT "068ff080b369adfac81509f9b57b2afabaf82dc5")

# SQLite amalgamation: persistence. Public domain.
set(MUSIC_DL_SQLITE_VERSION "3.54.1")
set(MUSIC_DL_SQLITE_URL "https://sqlite.org/2026/sqlite-amalgamation-3540100.zip")
set(MUSIC_DL_SQLITE_SHA3_256 "92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30")

# Catch2 v3: test framework, fetched only when BUILD_TESTING is ON. BSL-1.0.
set(MUSIC_DL_CATCH2_REPOSITORY "https://github.com/catchorg/Catch2.git")
set(MUSIC_DL_CATCH2_TAG "v3.16.1")
set(MUSIC_DL_CATCH2_COMMIT "08092139210881f01a34e60c5f2fd6afa0ea4024")
