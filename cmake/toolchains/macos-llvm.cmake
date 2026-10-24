# Native macOS builds with Homebrew LLVM 20 and its matching libc++.
# std::jthread / stop_token became non-experimental in libc++ 20; do not
# accidentally compile against an older SDK's C++ headers or runtime.
if(NOT CMAKE_HOST_APPLE)
    message(FATAL_ERROR "macos-llvm.cmake requires a macOS host")
endif()
if("$ENV{MUSIC_SERVICE_LLVM_ROOT}" STREQUAL "")
    message(FATAL_ERROR "Set MUSIC_SERVICE_LLVM_ROOT to the output of: brew --prefix llvm@20")
endif()
# CMake 4 leaves the SDK empty by default. Select one before compiler detection
# so FindIconv and Clang agree on the implicit system include directories. An
# explicit SDK /usr/include ahead of Clang's builtin headers breaks stddef.h.
# Preserve an SDK selected by the caller through the cache or SDKROOT.
if(NOT CMAKE_OSX_SYSROOT AND "$ENV{SDKROOT}" STREQUAL "")
    set(CMAKE_OSX_SYSROOT macosx CACHE STRING "macOS SDK" FORCE)
endif()
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS deployment target")
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
