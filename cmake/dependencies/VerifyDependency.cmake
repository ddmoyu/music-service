include_guard(GLOBAL)

find_package(Git REQUIRED)

# Fails the configure step unless the populated source tree is exactly the pinned
# release: a Git checkout whose HEAD equals the expected commit, with no modified,
# staged or untracked files, and with every submodule initialized at its recorded
# commit and unmodified. This also covers FETCHCONTENT_SOURCE_DIR_* overrides used for
# offline builds and reused build directories: a prepared source must still be the
# pinned commit, unchanged.
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

    # Same commit is not enough: edited, staged or added files change what is compiled.
    # --ignore-submodules=none also reports submodules at another commit or with edits.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" status --porcelain=v1
                --untracked-files=normal --ignore-submodules=none
        OUTPUT_VARIABLE changes
        ERROR_VARIABLE git_error
        RESULT_VARIABLE git_result
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT git_result EQUAL 0)
        message(FATAL_ERROR "Dependency ${name}: cannot inspect the working tree.\n${git_error}")
    endif()
    if(NOT changes STREQUAL "")
        message(FATAL_ERROR
            "Dependency ${name}: '${source_dir}' differs from commit ${expected_commit}:\n"
            "${changes}\nRemove the local changes or delete the source directory to fetch it again.")
    endif()

    # A submodule that was never checked out shows no changes above, so require each
    # recorded submodule to be initialized at its recorded commit.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" submodule status --recursive
        OUTPUT_VARIABLE submodules
        ERROR_VARIABLE git_error
        RESULT_VARIABLE git_result
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT git_result EQUAL 0)
        message(FATAL_ERROR "Dependency ${name}: cannot inspect submodules.\n${git_error}")
    endif()
    string(REPLACE "\n" ";" submodule_lines "${submodules}")
    foreach(line IN LISTS submodule_lines)
        if(line MATCHES "^[-+U]")
            message(FATAL_ERROR
                "Dependency ${name}: submodule is not at its recorded commit or not "
                "initialized:\n${line}")
        endif()
    endforeach()

    message(STATUS "Dependency ${name}: verified ${expected_commit}, clean working tree")
endfunction()

# Fails unless each listed file under source_dir has the expected SHA3-256.
# Entries are "relative/path=hash".
function(music_service_verify_file_hashes name source_dir)
    foreach(entry IN LISTS ARGN)
        string(REPLACE "=" ";" parts "${entry}")
        list(GET parts 0 relative)
        list(GET parts 1 expected)
        set(file "${source_dir}/${relative}")
        if(NOT EXISTS "${file}")
            message(FATAL_ERROR "Dependency ${name}: '${file}' does not exist.")
        endif()
        file(SHA3_256 "${file}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR
                "Dependency ${name}: ${relative} has SHA3-256 ${actual}, expected ${expected}. "
                "The source is not the pinned release.")
        endif()
    endforeach()
    message(STATUS "Dependency ${name}: verified file hashes")
endfunction()
