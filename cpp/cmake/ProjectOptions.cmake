include_guard(GLOBAL)

# Shared language requirements for every project target.
add_library(music_dl_project_options INTERFACE)
add_library(music_dl::project_options ALIAS music_dl_project_options)
target_compile_features(music_dl_project_options INTERFACE cxx_std_20)
if(MSVC)
    # UTF-8 sources and execution charset; standard-conforming __cplusplus.
    target_compile_options(music_dl_project_options INTERFACE
        /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor
    )
endif()

# Warnings apply to project code only, never to third-party sources.
add_library(music_dl_project_warnings INTERFACE)
add_library(music_dl::project_warnings ALIAS music_dl_project_warnings)
if(MSVC)
    target_compile_options(music_dl_project_warnings INTERFACE /W4)
else()
    target_compile_options(music_dl_project_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    )
endif()

option(MUSIC_DL_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
if(MUSIC_DL_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(music_dl_project_warnings INTERFACE /WX)
    else()
        target_compile_options(music_dl_project_warnings INTERFACE -Werror)
    endif()
endif()
