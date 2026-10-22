#include "audio_processing/tags/tag_writer.hpp"

#include <flacfile.h>
#include <mp4file.h>
#include <mpegfile.h>
#include <tpropertymap.h>
#include <tvariant.h>

#include <memory>
#include <utility>

namespace music_service::audio_processing::tags {

namespace {

using domain::Error;
using domain::ErrorCode;

Error failed(std::string reason, std::string message) {
    Error error{ErrorCode::metadata_failed, std::move(message), false, {}};
    error.details.push_back({"tags", std::move(reason)});
    return error;
}

TagLib::String utf8(const std::string& text) { return TagLib::String(text, TagLib::String::UTF8); }

// The items to write as unified properties; the keys every TagLib format maps.
TagLib::PropertyMap wanted(const application::ports::TrackMetadata& metadata) {
    TagLib::PropertyMap map;
    if (!metadata.title.empty()) {
        map.insert("TITLE", TagLib::StringList(utf8(metadata.title)));
    }
    TagLib::StringList artists;
    for (const auto& artist : metadata.artists) {
        if (!artist.empty()) {
            artists.append(utf8(artist));
        }
    }
    if (!artists.isEmpty()) {
        map.insert("ARTIST", artists);
    }
    if (metadata.album && !metadata.album->empty()) {
        map.insert("ALBUM", TagLib::StringList(utf8(*metadata.album)));
    }
    if (metadata.lyrics && !metadata.lyrics->empty()) {
        map.insert("LYRICS", TagLib::StringList(utf8(*metadata.lyrics)));
    }
    if (metadata.track_number && *metadata.track_number > 0) {
        auto number = std::to_string(*metadata.track_number);
        if (metadata.track_total && *metadata.track_total >= *metadata.track_number) {
            number += "/" + std::to_string(*metadata.track_total);
        }
        map.insert("TRACKNUMBER", TagLib::StringList(utf8(number)));
    }
    return map;
}

std::string item_name(const TagLib::String& key) {
    if (key == "TITLE") {
        return "title";
    }
    if (key == "ARTIST") {
        return "artist";
    }
    if (key == "ALBUM") {
        return "album";
    }
    if (key == "TRACKNUMBER") {
        return "track_number";
    }
    return "lyrics";
}

std::unique_ptr<TagLib::File> open(const std::filesystem::path& file, domain::Container container) {
    switch (container) {
    case domain::Container::mp3:
        return std::make_unique<TagLib::MPEG::File>(file.c_str(), false);
    case domain::Container::flac:
        return std::make_unique<TagLib::FLAC::File>(file.c_str(), false);
    case domain::Container::mp4:
        return std::make_unique<TagLib::MP4::File>(file.c_str(), false);
    default:
        return nullptr;
    }
}

bool save(TagLib::File& file, domain::Container container) {
    if (container == domain::Container::mp3) {
        // Only the ID3v2 tag is written; an ID3v1 or APE tag already there stays as it was.
        return static_cast<TagLib::MPEG::File&>(file).save(TagLib::MPEG::File::ID3v2, TagLib::File::StripNone,
                                                            TagLib::ID3v2::v4, TagLib::File::DoNotDuplicate);
    }
    return file.save();
}

} // namespace

domain::Result<TagWriteResult> write_tags(const std::filesystem::path& file, domain::Container container,
                                          const application::ports::TrackMetadata& metadata) {
    auto opened = open(file, container);
    if (!opened) {
        Error error{ErrorCode::capability_unsupported, "tags cannot be written into this container", false, {}};
        error.details.push_back({"container", std::string{domain::to_string(container)}});
        return error;
    }
    if (!opened->isValid() || opened->readOnly()) {
        return failed("open", "the audio file could not be opened for tagging");
    }
    const auto items = wanted(metadata);
    auto properties = opened->properties();
    for (const auto& [key, values] : items) {
        properties.replace(key, values);
    }
    TagWriteResult result;
    // Keys the format cannot hold come back unsupported; everything else is kept as it was.
    const auto unsupported = opened->setProperties(properties);
    for (const auto& [key, values] : items) {
        if (unsupported.contains(key)) {
            result.missing.push_back(item_name(key));
        }
    }
    bool cover_set = true;
    if (metadata.cover && !metadata.cover->data.empty()) {
        TagLib::VariantMap picture;
        picture.insert("data", TagLib::ByteVector(metadata.cover->data.data(),
                                                  static_cast<unsigned int>(metadata.cover->data.size())));
        picture.insert("mimeType", utf8(metadata.cover->mime_type));
        picture.insert("description", TagLib::String());
        picture.insert("pictureType", TagLib::String("Front Cover"));
        cover_set = opened->setComplexProperties("PICTURE", TagLib::List<TagLib::VariantMap>{picture});
        if (!cover_set) {
            result.missing.push_back("cover");
        }
    }
    if (!save(*opened, container)) {
        return failed("save", "the tags could not be saved into the audio file");
    }
    opened.reset();

    // Read back what was written, from the saved bytes.
    auto reread = open(file, container);
    if (!reread || !reread->isValid()) {
        return failed("reread", "the tagged file could not be read back");
    }
    const auto saved = reread->properties();
    for (const auto& [key, values] : items) {
        if (unsupported.contains(key)) {
            continue;
        }
        if (!saved.contains(key) || saved[key] != values) {
            return failed("verify", "a written tag did not read back: " + item_name(key));
        }
    }
    if (metadata.cover && !metadata.cover->data.empty() && cover_set) {
        const auto pictures = reread->complexProperties("PICTURE");
        const TagLib::ByteVector expected(metadata.cover->data.data(),
                                          static_cast<unsigned int>(metadata.cover->data.size()));
        bool found = false;
        for (const auto& picture : pictures) {
            found = found || picture.value("data").toByteVector() == expected;
        }
        if (!found) {
            return failed("verify", "the written cover did not read back");
        }
    }
    return result;
}

} // namespace music_service::audio_processing::tags
