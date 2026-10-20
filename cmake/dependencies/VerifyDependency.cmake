include_guard(GLOBAL)

find_package(Git REQUIRED)

# Fails the configure step unless the populated source tree is a Git checkout
# whose HEAD equals the expected commit. This also covers FETCHCONTENT_SOURCE_DIR_*
# overrides used for offline builds: a prepared source must still be the pinned commit.
function(music_service_verify_git_dependency name source_dir expected_commit)
    if(NOT EXISTS "${source_dir}")
        message(FATAL_ERROR "Dependency ${name}: source directory '${source_dir}' does not exist.")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse HEAD
        OUTPUT_VARIABLE actual_commit
        ERROR_VARIABLE git_error
        RESULT_VARIABLE git_result
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT git_result EQUAL 0)
        message(FATAL_ERROR
            "Dependency ${name}: '${source_dir}' is not a Git checkout, so its version "
            "cannot be verified.\n${git_error}")
    endif()

    if(NOT actual_commit STREQUAL expected_commit)
        message(FATAL_ERROR
            "Dependency ${name}: HEAD is ${actual_commit}, expected ${expected_commit}. "
            "The tag may have moved or the source override is at another version.")
    endif()

    message(STATUS "Dependency ${name}: verified ${expected_commit}")
endfunction()
