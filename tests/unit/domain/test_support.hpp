#pragma once

#include "domain/catalog/refs.hpp"
#include "domain/identity/account.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace music_service::test {

template <class T> T unwrap(domain::Result<T> result) {
    REQUIRE(result.has_value());
    return std::move(result).value();
}

inline domain::SourceId source(std::string_view text) {
    return unwrap(domain::SourceId::parse(text));
}

inline domain::TrackRef track(std::string_view src, std::string_view id) {
    return {source(src), unwrap(domain::PlatformId::parse(id))};
}

inline domain::AccountBinding anonymous(std::string principal, std::string_view src) {
    return unwrap(domain::AccountBinding::make(std::move(principal), source(src),
                                               domain::AccountMode::anonymous, "anonymous",
                                               std::nullopt, std::nullopt));
}

inline domain::AccountBinding user(std::string principal, std::string_view src,
                                   std::string credential_id) {
    return unwrap(domain::AccountBinding::make(std::move(principal), source(src),
                                               domain::AccountMode::user, "cookie",
                                               std::move(credential_id), std::nullopt));
}

} // namespace music_service::test
