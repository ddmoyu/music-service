cmake_minimum_required(VERSION 3.30)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build, for example a CMake preset.")
endif()

project(music_service
    VERSION 0.1.29
    DESCRIPTION "Cross-platform music backend libraries and service"
    LANGUAGES C CXX
)

include(cmake/MsvcDependencies.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The project does not use C++20 modules; skip dependency scanning.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(BUILD_TESTING "Build project tests" OFF)

include(cmake/ProjectOptions.cmake)
include(CTest)
include(cmake/dependencies/Dependencies.cmake)

add_subdirectory(src)
include(cmake/Packaging.cmake)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
