include_guard(GLOBAL)

# Single source of truth for third-party versions.
# Git dependencies: release tag + expected commit (the peeled tag target).
# Archive dependencies: release URL + published hash.
# Update a tag and its commit together; a mismatch stops the configure step.

# cpp-httplib: HTTP server/client, header only. MIT.
set(MUSIC_SERVICE_HTTPLIB_REPOSITORY "https://github.com/yhirose/cpp-httplib.git")
set(MUSIC_SERVICE_HTTPLIB_TAG "v0.62.0")
set(MUSIC_SERVICE_HTTPLIB_COMMIT "74ef3c42188acd6391f027c4337fb0e1a68c0024")

# nlohmann/json: JSON at protocol boundaries. MIT.
set(MUSIC_SERVICE_JSON_REPOSITORY "https://github.com/nlohmann/json.git")
set(MUSIC_SERVICE_JSON_TAG "v3.12.0")
set(MUSIC_SERVICE_JSON_COMMIT "55f93686c01528224f448c19128836e7df245f72")

# Mbed TLS 3.6 LTS: TLS backend for cpp-httplib. Apache-2.0 OR GPL-2.0-or-later.
set(MUSIC_SERVICE_MBEDTLS_REPOSITORY "https://github.com/Mbed-TLS/mbedtls.git")
set(MUSIC_SERVICE_MBEDTLS_TAG "v3.6.7")
set(MUSIC_SERVICE_MBEDTLS_COMMIT "068ff080b369adfac81509f9b57b2afabaf82dc5")

# SQLite amalgamation: persistence. Public domain.
set(MUSIC_SERVICE_SQLITE_VERSION "3.54.1")
set(MUSIC_SERVICE_SQLITE_URL "https://sqlite.org/2026/sqlite-amalgamation-3540100.zip")
set(MUSIC_SERVICE_SQLITE_SHA3_256 "92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30")
# Hashes of the compiled files inside that archive. They are checked on every configure,
# including FETCHCONTENT_SOURCE_DIR_MUSIC_SERVICE_SQLITE overrides, which skip the
# archive download and its hash.
set(MUSIC_SERVICE_SQLITE_FILE_SHA3_256
    "sqlite3.c=e2b869d3003b33c942b7dd7063604fbc53553e1b66609570df39b9b7b558ee81"
    "sqlite3.h=15ef3f5dbc4a7a980f828fa0d1e473cee5a124e7bb77e9bc70fc0bfa3f9a2af8"
)

# TagLib: audio tags and properties (MP3, FLAC, MP4/M4A); its utfcpp submodule is
# pinned by the tag. Dual LGPL-2.1/MPL-1.1, used under MPL-1.1 (static linking).
set(MUSIC_SERVICE_TAGLIB_REPOSITORY "https://github.com/taglib/taglib.git")
set(MUSIC_SERVICE_TAGLIB_TAG "v2.3.2")
set(MUSIC_SERVICE_TAGLIB_COMMIT "deadc2990767dfbda0701e0ab35fdeea653db08f")

# Catch2 v3: test framework, fetched only when BUILD_TESTING is ON. BSL-1.0.
set(MUSIC_SERVICE_CATCH2_REPOSITORY "https://github.com/catchorg/Catch2.git")
set(MUSIC_SERVICE_CATCH2_TAG "v3.16.1")
set(MUSIC_SERVICE_CATCH2_COMMIT "08092139210881f01a34e60c5f2fd6afa0ea4024")
