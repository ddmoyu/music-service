# AudioProcessor implementations: container identification and structure validation
# (format/) and tag writing with TagLib (tags/). Decryption joins with the platforms that
# need it (the AudioDecryptor port). Mbed TLS hashes the validated bytes.
add_library(music_service_audio_processing STATIC
    format/byte_reader.cpp
    format/flac.cpp
    format/mp3.cpp
    format/mp4.cpp
    tags/tag_writer.cpp
    verifying_processor.cpp
)
add_library(music_service::audio_processing ALIAS music_service_audio_processing)
target_link_libraries(music_service_audio_processing
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        music_service::third_party_taglib
        mbedcrypto
)
