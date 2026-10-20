include_guard(GLOBAL)

# SQLite ships an amalgamation without a CMake build; compile it as a static library.

find_package(Threads REQUIRED)

add_library(music_dl_third_party_sqlite STATIC "${music_dl_sqlite_SOURCE_DIR}/sqlite3.c")
add_library(music_dl::third_party_sqlite ALIAS music_dl_third_party_sqlite)
set_target_properties(music_dl_third_party_sqlite PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(music_dl_third_party_sqlite SYSTEM PUBLIC
    "${music_dl_sqlite_SOURCE_DIR}"
)
target_compile_definitions(music_dl_third_party_sqlite PRIVATE
    SQLITE_DQS=0
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_SHARED_CACHE
)
target_link_libraries(music_dl_third_party_sqlite PUBLIC Threads::Threads)
if(UNIX)
    target_link_libraries(music_dl_third_party_sqlite PUBLIC m)
endif()
