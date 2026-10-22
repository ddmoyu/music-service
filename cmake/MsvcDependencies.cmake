# Ninja matches /showIncludes as bytes. Some bundled CMake versions decode a
# Chinese-only MSVC installation's UTF-8 prefix using the system ANSI codepage,
# silently recording zero dependencies. Probe without transcoding, even when
# VSLANG=1033 has to fall back to the only installed language pack.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    set(dep_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/music-service-deps")
    file(MAKE_DIRECTORY "${dep_probe_dir}")
    set(dep_probe_header "${dep_probe_dir}/music_service_dep_probe.hpp")
    file(WRITE "${dep_probe_header}" "// include dependency probe\n")
    file(WRITE "${dep_probe_dir}/probe.cpp" "#include \"${dep_probe_header}\"\n")
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c
            "${dep_probe_dir}/probe.cpp" "/Fo${dep_probe_dir}/probe.obj"
        RESULT_VARIABLE dep_probe_result
        OUTPUT_VARIABLE dep_probe_output ERROR_VARIABLE dep_probe_error
        ENCODING NONE)
    file(TO_NATIVE_PATH "${dep_probe_header}" dep_probe_native)
    string(REPLACE "\r" "" dep_probe_output "${dep_probe_output}")
    string(REPLACE "\n" ";" dep_probe_lines "${dep_probe_output}")
    set(dep_probe_prefix "")
    foreach(dep_probe_line IN LISTS dep_probe_lines)
        string(FIND "${dep_probe_line}" "${dep_probe_native}" dep_probe_pos)
        if(dep_probe_pos LESS 0)
            string(FIND "${dep_probe_line}" "${dep_probe_header}" dep_probe_pos)
        endif()
        if(dep_probe_pos GREATER 0)
            string(SUBSTRING "${dep_probe_line}" 0 ${dep_probe_pos} dep_probe_prefix)
            break()
        endif()
    endforeach()
    if(NOT dep_probe_result EQUAL 0 OR dep_probe_prefix STREQUAL "")
        message(FATAL_ERROR "Cannot verify MSVC header dependency prefix: ${dep_probe_error}")
    endif()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${dep_probe_prefix}")
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "${dep_probe_prefix}")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${dep_probe_prefix}")
endif()
