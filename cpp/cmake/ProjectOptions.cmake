include_guard(GLOBAL)

# Shared language requirements only; compiler warnings belong to project targets.
add_library(music_dl_project_options INTERFACE)
add_library(music_dl::project_options ALIAS music_dl_project_options)
target_compile_features(music_dl_project_options INTERFACE cxx_std_20)
