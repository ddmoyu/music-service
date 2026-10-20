# Executors, the unified upstream admission ledger, deadlines and graceful shutdown.
# No knowledge of songs, tasks or platform errors.
add_library(music_service_runtime STATIC
    admission/admission_ledger.cpp
    cancellation/deadline_timer.cpp
    executors/bounded_executor.cpp
    shutdown/shutdown_sequence.cpp
)
add_library(music_service::runtime ALIAS music_service_runtime)
find_package(Threads REQUIRED)
target_link_libraries(music_service_runtime
    PUBLIC
        music_service::application
        Threads::Threads
    PRIVATE
        music_service::project_warnings
)
