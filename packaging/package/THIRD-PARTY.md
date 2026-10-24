# Third-party software

music-service is statically linked with the following libraries. Their license texts are
in this directory. Versions are pinned in `cmake/dependencies/Versions.cmake` of the source.

| Library | Use | License | File |
| --- | --- | --- | --- |
| Mbed TLS | TLS and hashes | Apache-2.0 (or GPL-2.0-or-later) | `mbedtls-LICENSE.txt` |
| cpp-httplib | HTTP server and client | MIT | `cpp-httplib-LICENSE.txt` |
| nlohmann/json | JSON | MIT | `nlohmann-json-LICENSE.txt` |
| TagLib | audio tags | MPL-1.1 (used under MPL; source available from the TagLib project) | `taglib-COPYING.MPL.txt` |
| utfcpp | UTF-8 (part of TagLib) | BSL-1.0 | `utfcpp-LICENSE.txt` |
| zlib | lyric decompression | zlib | `zlib-LICENSE.txt` |
| SQLite | database | Public domain | none needed |

The native MG3D key-recovery algorithm in `audio_processing/migu_z3d.cpp` adapts
Unlock Music's MIT-licensed approach (Copyright 2019-2023 MengYX). Its notice is
included as `unlock-music-LICENSE.txt`. Reference source:
[mg3d.ts, revision f27bc0478555d8de4b068205f6d15d17edd4e0ca](https://github.com/DHJComical/unlock-music-web-mirror/blob/f27bc0478555d8de4b068205f6d15d17edd4e0ca/src/decrypt/mg3d.ts).
This is compiled C++ code; the JavaScript application is not bundled or invoked.

The Windows program also contains the Microsoft C/C++ runtime, linked statically under the
Visual Studio license terms for redistributable code.

macOS archives also contain the LLVM 20 runtime libraries actually referenced by the
program (libc++, libc++abi and, when linked, libunwind), relocated under
`lib/music-service/`. They are the existing build toolchain's runtimes, not a bundled
compiler. The full component licenses from the `llvmorg-20.1.8` source tag accompany
macOS packages as `libcxx-LICENSE.txt`, `libcxxabi-LICENSE.txt` and
`libunwind-LICENSE.txt`; they include Apache-2.0 with LLVM exceptions and retained
component notices. System libraries and Apple frameworks are not copied.

License sources:
[libc++](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/libcxx/LICENSE.TXT),
[libc++abi](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/libcxxabi/LICENSE.TXT),
[libunwind](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.8/libunwind/LICENSE.TXT).
