#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace music_service::infrastructure::observability {

// Client-supplied X-Request-Id: 1..64 characters of [A-Za-z0-9._-].
[[nodiscard]] bool is_valid_client_request_id(std::string_view id) noexcept;

// Generates opaque service ids such as "req_..." or "tsk_...". Not for secrets:
// tokens and media tickets need a cryptographic generator.
class IdGenerator {
public:
    IdGenerator();
    explicit IdGenerator(std::uint64_t seed); // deterministic, for tests

    // prefix + 20 characters from [A-Za-z0-9].
    [[nodiscard]] std::string next(std::string_view prefix);

private:
    std::mutex mutex_;
    std::mt19937_64 engine_;
};

// Keeps a valid client request id, otherwise generates a new "req_" id.
[[nodiscard]] std::string accept_or_generate_request_id(std::optional<std::string_view> client_id,
                                                        IdGenerator& generator);

} // namespace music_service::infrastructure::observability
