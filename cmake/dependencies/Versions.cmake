include_guard(GLOBAL)

# 酷我歌词的有界解压；官方发布归档及 SHA-256，zlib 许可。
set(MUSIC_SERVICE_ZLIB_URLS
    "https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz"
    "https://zlib.net/fossils/zlib-1.3.2.tar.gz"
)
set(MUSIC_SERVICE_ZLIB_SHA256 "bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16")

# Single source of truth for third-party versions.
# Git dependencies: release tag + expected commit (the peeled tag target).
# Archive dependencies: release URL + published hash.
# Update a tag and its commit together; a mismatch stops the configure step.

# cpp-httplib: HTTP server/client, header only. MIT.
set(MUSIC_SERVICE_HTTPLIB_REPOSITORY "https://github.com/yhirose/cpp-httplib.git")
set(MUSIC_SERVICE_HTTPLIB_TAG "v0.62.0")
set(MUSIC_SERVICE_HTTPLIB_COMMIT "74ef3c42188acd6391f027c4337fb0e1a68c0024")

# nlohmann/json: JSON at protocol boundaries. MIT.
set(MUSIC_SERVICE_JSON_REPOSITORY "https://github.com/nlohmann/json.git")
set(MUSIC_SERVICE_JSON_TAG "v3.12.0")
set(MUSIC_SERVICE_JSON_COMMIT "55f93686c01528224f448c19128836e7df245f72")

# Mbed TLS 3.6 LTS: TLS backend for cpp-httplib. Apache-2.0 OR GPL-2.0-or-later.
set(MUSIC_SERVICE_MBEDTLS_REPOSITORY "https://github.com/Mbed-TLS/mbedtls.git")
set(MUSIC_SERVICE_MBEDTLS_TAG "v3.6.7")
set(MUSIC_SERVICE_MBEDTLS_COMMIT "068ff080b369adfac81509f9b57b2afabaf82dc5")

# SQLite amalgamation: persistence. Public domain.
set(MUSIC_SERVICE_SQLITE_VERSION "3.54.1")
set(MUSIC_SERVICE_SQLITE_URL "https://sqlite.org/2026/sqlite-amalgamation-3540100.zip")
set(MUSIC_SERVICE_SQLITE_SHA3_256 "92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30")
# Hashes of the compiled files inside that archive. They are checked on every configure,
# including FETCHCONTENT_SOURCE_DIR_MUSIC_SERVICE_SQLITE overrides, which skip the
# archive download and its hash.
set(MUSIC_SERVICE_SQLITE_FILE_SHA3_256
    "sqlite3.c=e2b869d3003b33c942b7dd7063604fbc53553e1b66609570df39b9b7b558ee81"
    "sqlite3.h=15ef3f5dbc4a7a980f828fa0d1e473cee5a124e7bb77e9bc70fc0bfa3f9a2af8"
)

# TagLib: audio tags and properties (MP3, FLAC, MP4/M4A); its utfcpp submodule is
# pinned by the tag. Dual LGPL-2.1/MPL-1.1, used under MPL-1.1 (static linking).
set(MUSIC_SERVICE_TAGLIB_REPOSITORY "https://github.com/taglib/taglib.git")
set(MUSIC_SERVICE_TAGLIB_TAG "v2.3.2")
set(MUSIC_SERVICE_TAGLIB_COMMIT "deadc2990767dfbda0701e0ab35fdeea653db08f")

# Catch2 v3: test framework, fetched only when BUILD_TESTING is ON. BSL-1.0.
set(MUSIC_SERVICE_CATCH2_REPOSITORY "https://github.com/catchorg/Catch2.git")
set(MUSIC_SERVICE_CATCH2_TAG "v3.16.1")
set(MUSIC_SERVICE_CATCH2_COMMIT "08092139210881f01a34e60c5f2fd6afa0ea4024")

# zlib 官方归档中的构建输入；源码覆盖同样校验。
set(MUSIC_SERVICE_ZLIB_FILE_SHA3_256
    "zutil.h=f5ce3c36dfadc8cfbec058dbf8f0fdf76676171648a09007be5829ca06185fec"
    "inftrees.h=2262992da2d5718576133e33014acb88f87913235efa22687bd3aa2f86071729"
    "inflate.c=b1b5804b1d5cac70bc2844d37d2ce05fd104dd522091cca9dd2fd5ddd5ac471b"
    "CMakeLists.txt=059bda2a2fa84de32885b0c2b4752111048659cd5096768ef0db61e97ba5444e"
    "zlib.pc.cmakein=342d20e9c5381efb5bcdc052ca83283715291f9c947dc085f572b5e98b8da2f2"
    "LICENSE=b4663005c9cd91f86bf77b4d588b8ad822a43998741f33c0b5534ed84f20794c"
    "compress.c=a1d41c2199a9e58425a77cd2b2983011a4872aa28b9b5f7a752ba46214920c35"
    "deflate.c=144dfda58fa25898ec68ff065ba0638e124f01678f651846727bef794a95157e"
    "inffixed.h=4294b721f27264d380528e937be311413ccd7d9fe6e77f45de5592a9f036fa10"
    "gzread.c=4507a892b8b7494e07f3ae112c38e88d8f1dc152d21792abd0519de3538e457d"
    "trees.h=1f5e27af3b5fbbce355cd7ab95c2f4433afef523e38e5895920624b33f0e485a"
    "inffast.h=80dc927329edbb8b717d16e730134b370b687bfea33ec73e469fbee5adb8c2a8"
    "crc32.c=6a3ed75c23e090d47bf265d688d6b208c8d1aca70f924c931645e7ac66c21661"
    "infback.c=2750ce0aaf03d6e0391d0ca1b4ed15321444686cf260fd4d975b8325343e0c5e"
    "zutil.c=c697f21f8c76dde7a96f3cae35dcad0b0cf30c5618ece7cc3b2806545eff261a"
    "deflate.h=bac0b2e84c0917ece2e34a4128a900ff4e4a6a6173f34dad0a8ab3ea0b00cc46"
    "gzguts.h=704ab61a26bcb1b71c33ecdd0ce2f714a093a26513158a9edd7f3c58a5d2978d"
    "zlib.h=1fdfa281656995ab708acb6cb80c8db2aa2ec4bce7beda0fce19b312ee03a1ff"
    "gzlib.c=6a5d2696d5ba70be65f45662f4901c47c95734e7bb4a9c9c9c527949c00f7df3"
    "inflate.h=eaaa758461ba4a611af74ccf82bcf094abaf58b437257bd2b86997feb71434be"
    "inftrees.c=78a69c091f6abfcd8b408479c5514a7b33eba234eebcd105cb0b827aec4baf78"
    "uncompr.c=c41f5dd558ede310493d1ffbf4e7eb1a90d6bc0e911a32cc37c5cfa474818240"
    "gzwrite.c=78ac5888f1f889dd0e30e6ba67b5266d5a94076daef43a5de5ee0ea29e3dde6c"
    "trees.c=6dece2effaf41f1e6da2ed8efbb57c2686c77fecb00a2e2b935bd8f8cd8ad8d9"
    "gzclose.c=4850d604f40ec00957265102d006010b43e100dc2c9943eedb6a234b1e86a0ca"
    "crc32.h=dedfbe002f4d77cb6699d13a400fcca406232fc2d173e5a7b601824e4b451dfb"
    "inffast.c=41128c7f090f01c9eff92432d5951a0b3a5341dbfa8e06e2227e37e304861bba"
    "adler32.c=42a1a3da42b7ce9eb56a73fb004f5cfc04d6554e8558d0416e9fedab01702f03"
    "zconf.h=1aea1c7b423eb175108e79b3fdcb6f3de4ec86399cdc877b4a232bbab39f9fda"
)
