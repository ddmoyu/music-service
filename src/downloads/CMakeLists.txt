# Download execution: publication of verified files (Finalizer), startup recovery of
# publications and cancels, and the resume decision for partial files. The scheduler and
# runner join with T17. JSON only encodes the evidence and results kept as opaque text.
add_library(music_service_downloads STATIC
    finalizer.cpp
    recovery.cpp
    resume_policy.cpp
)
add_library(music_service::downloads ALIAS music_service_downloads)
target_link_libraries(music_service_downloads
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        nlohmann_json::nlohmann_json
)
