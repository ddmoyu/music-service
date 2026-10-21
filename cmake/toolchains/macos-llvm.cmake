# Native macOS builds with Homebrew LLVM 20 and its matching libc++.
# std::jthread / stop_token became non-experimental in libc++ 20; do not
# accidentally compile against an older SDK's C++ headers or runtime.
if(NOT CMAKE_HOST_APPLE)
    message(FATAL_ERROR "macos-llvm.cmake requires a macOS host")
endif()
if("$ENV{MUSIC_SERVICE_LLVM_ROOT}" STREQUAL "")
    message(FATAL_ERROR "Set MUSIC_SERVICE_LLVM_ROOT to the output of: brew --prefix llvm@20")
endif()
set(music_service_llvm_root "$ENV{MUSIC_SERVICE_LLVM_ROOT}")
set(CMAKE_C_COMPILER "${music_service_llvm_root}/bin/clang")
set(CMAKE_CXX_COMPILER "${music_service_llvm_root}/bin/clang++")
# Keep Darwin archive metadata compatible with Apple's linker, including the
# mostly-empty optional Mbed TLS libraries. LLVM's inferred archive format can
# otherwise leave a GNU '/' symbol-table member that ld rejects as non-Mach-O.
set(CMAKE_AR "/usr/bin/ar" CACHE FILEPATH "Apple archive tool" FORCE)
set(CMAKE_RANLIB "/usr/bin/ranlib" CACHE FILEPATH "Apple archive index tool" FORCE)
set(CMAKE_CXX_FLAGS_INIT
    "-stdlib=libc++ -nostdinc++ -isystem \"${music_service_llvm_root}/include/c++/v1\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-L\"${music_service_llvm_root}/lib/c++\" -Wl,-rpath,\"${music_service_llvm_root}/lib/c++\"")
