# Download execution: the persistent scheduler and the runner (T17), publication of
# verified files (Finalizer), startup recovery of publications and cancels, and the resume
# decision for partial files, and the cleanup of work directories and expired
# idempotency records. JSON only encodes the evidence, progress and results kept as
# opaque text.
add_library(music_service_downloads STATIC
    finalizer.cpp
    maintenance.cpp
    recovery.cpp
    resume_policy.cpp
    runner.cpp
    scheduler.cpp
)
add_library(music_service::downloads ALIAS music_service_downloads)
target_link_libraries(music_service_downloads
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        nlohmann_json::nlohmann_json
)
