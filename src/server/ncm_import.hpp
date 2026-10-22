#pragma once
#include "server/instance.hpp"
#include "application/ports/processing/audio_processor.hpp"
namespace music_service::server {
domain::Result<application::ports::CompletedPublication> import_ncm(
    const application::config::ServiceConfig& config, const std::filesystem::path& input,
    const std::string& owner, const std::string& destination, domain::MetadataPolicy policy,
    const application::ports::OperationContext& context);
}
