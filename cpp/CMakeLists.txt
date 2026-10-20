cmake_minimum_required(VERSION 3.30)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build, for example a CMake preset.")
endif()

project(music_dl
    VERSION 0.1.0
    DESCRIPTION "Cross-platform music backend libraries and service"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTING "Enable project tests when test sources are added" OFF)

include(cmake/ProjectOptions.cmake)
include(CTest)

add_subdirectory(src)
add_subdirectory(apps/server)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

message(STATUS "Scaffold only: no C++ sources, providers, or server binary have been implemented.")
