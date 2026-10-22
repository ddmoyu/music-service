include_guard(GLOBAL)

# The static MSVC runtime for every target, third-party ones included: the program needs
# no Visual C++ Redistributable on the machine it runs on.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Shared language requirements for every project target.
add_library(music_service_project_options INTERFACE)
add_library(music_service::project_options ALIAS music_service_project_options)
target_compile_features(music_service_project_options INTERFACE cxx_std_20)
if(MSVC)
    # UTF-8 sources and execution charset; standard-conforming __cplusplus.
    target_compile_options(music_service_project_options INTERFACE
        /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor
    )
endif()

# Warnings apply to project code only, never to third-party sources.
add_library(music_service_project_warnings INTERFACE)
add_library(music_service::project_warnings ALIAS music_service_project_warnings)
if(MSVC)
    target_compile_options(music_service_project_warnings INTERFACE /W4)
else()
    target_compile_options(music_service_project_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    )
endif()

option(MUSIC_SERVICE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
if(MUSIC_SERVICE_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(music_service_project_warnings INTERFACE /WX)
    else()
        target_compile_options(music_service_project_warnings INTERFACE -Werror)
    endif()
endif()
