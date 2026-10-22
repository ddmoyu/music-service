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
| SQLite | database | Public domain | none needed |

The Windows program also contains the Microsoft C/C++ runtime, linked statically under the
Visual Studio license terms for redistributable code.
