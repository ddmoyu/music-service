# AudioProcessor implementations: container identification and structure validation
# (format/). Decryption and tag writing join with the platforms and T30. Mbed TLS hashes
# the validated bytes.
add_library(music_service_audio_processing STATIC
    format/byte_reader.cpp
    format/mp3.cpp
    verifying_processor.cpp
)
add_library(music_service::audio_processing ALIAS music_service_audio_processing)
target_link_libraries(music_service_audio_processing
    PUBLIC
        music_service::application
    PRIVATE
        music_service::project_warnings
        mbedcrypto
)
