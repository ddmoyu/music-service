#include "infrastructure/observability/correlation.hpp"

#include <array>

namespace music_dl::infrastructure::observability {

namespace {

constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

std::uint64_t random_seed() {
    std::random_device device;
    return (static_cast<std::uint64_t>(device()) << 32U) ^ device();
}

} // namespace

bool is_valid_client_request_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

IdGenerator::IdGenerator() : engine_(random_seed()) {}

IdGenerator::IdGenerator(std::uint64_t seed) : engine_(seed) {}

std::string IdGenerator::next(std::string_view prefix) {
    std::uniform_int_distribution<std::size_t> pick{0, alphabet.size() - 1};
    std::string id{prefix};
    const std::lock_guard lock{mutex_};
    for (int i = 0; i < 20; ++i) {
        id.push_back(alphabet[pick(engine_)]);
    }
    return id;
}

std::string accept_or_generate_request_id(std::optional<std::string_view> client_id,
                                          IdGenerator& generator) {
    if (client_id && is_valid_client_request_id(*client_id)) {
        return std::string{*client_id};
    }
    return generator.next("req_");
}

} // namespace music_dl::infrastructure::observability
