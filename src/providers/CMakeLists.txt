# Platform packages selected for this build. Each listed source has a subdirectory
# defining music_service_provider_<source> and providers/<source>/bundle.hpp with
# make_bundle(). The registry references those functions from a generated file, so
# no registration depends on static constructors surviving the linker.
set(MUSIC_SERVICE_PROVIDERS "kuwo;fivesing" CACHE STRING "Platform packages to build in (list of sources)")

set(provider_includes "")
set(provider_calls "")
set(provider_targets "")
foreach(source IN LISTS MUSIC_SERVICE_PROVIDERS)
    if(NOT source MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "MUSIC_SERVICE_PROVIDERS: invalid source '${source}'")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}/CMakeLists.txt")
        message(FATAL_ERROR "MUSIC_SERVICE_PROVIDERS: no platform package for '${source}'")
    endif()
    add_subdirectory(${source})
    string(APPEND provider_includes "#include \"providers/${source}/bundle.hpp\"\n")
    string(APPEND provider_calls "    bundles.push_back(${source}::make_bundle());\n")
    list(APPEND provider_targets music_service_provider_${source})
endforeach()
list(REMOVE_DUPLICATES provider_targets)
set(MUSIC_SERVICE_PROVIDER_INCLUDES "${provider_includes}")
set(MUSIC_SERVICE_PROVIDER_CALLS "${provider_calls}")
configure_file(registry/builtin_bundles.cpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_bundles.cpp" @ONLY)

# Registry, contract validation and the selected platform packages.
add_library(music_service_providers STATIC
    registry/provider_registry.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_bundles.cpp"
)
add_library(music_service::providers ALIAS music_service_providers)
target_link_libraries(music_service_providers
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        ${provider_targets}
)

# The helper runner is built in every configuration; external_adapters.enabled turns
# it on at run time. Turning the option off removes the capability from the build.
option(MUSIC_SERVICE_EXTERNAL_ADAPTERS "Build support for external helper programs (external_adapters)" ON)
if(MUSIC_SERVICE_EXTERNAL_ADAPTERS)
    add_subdirectory(external)
endif()
