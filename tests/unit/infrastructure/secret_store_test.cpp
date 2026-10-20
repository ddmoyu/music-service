#include "infrastructure/filesystem/data_layout.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/secrets/file_secret_store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <ostream>
#include <string>
#include <type_traits>

using namespace music_service;
using application::ports::SecretBytes;
using infrastructure::secrets::FileSecretStore;
namespace fs = std::filesystem;

namespace {

template <class T, class = void> struct is_streamable : std::false_type {};
template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<T>())>>
    : std::true_type {};

// SecretBytes must not be printable or copyable by accident.
static_assert(!is_streamable<const SecretBytes&>::value);
static_assert(!std::is_copy_constructible_v<SecretBytes>);
static_assert(!std::is_convertible_v<SecretBytes, std::string>);

struct Fixture {
    Fixture() {
        root = fs::temp_directory_path() / infrastructure::filesystem::path_from_utf8("music-service-秘密-test");
        fs::remove_all(root);
        layout = infrastructure::filesystem::prepare_data_dir(root).value();
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path root;
    infrastructure::filesystem::DataLayout layout;
};

} // namespace

TEST_CASE("secrets are stored under new immutable references", "[secrets]") {
    Fixture f;
    FileSecretStore store{f.layout.credentials};

    const auto first = store.put(SecretBytes{"MUSIC_U=cookie-value"});
    REQUIRE(first);
    CHECK(first->rfind("sec_", 0) == 0);

    const auto second = store.put(SecretBytes{"MUSIC_U=cookie-value"});
    REQUIRE(second);
    CHECK(*second != *first); // an update always gets a new reference

    const auto read = store.get(*first);
    REQUIRE(read);
    CHECK(read->reveal() == "MUSIC_U=cookie-value");
    CHECK(read->describe() == "[secret 20 bytes]");

    auto refs = store.list();
    REQUIRE(refs);
    std::sort(refs->begin(), refs->end());
    CHECK(refs->size() == 2);

    REQUIRE(store.remove(*first));
    CHECK(store.get(*first).error().code == domain::ErrorCode::not_found);
    CHECK(store.remove(*first)); // repeated cleanup succeeds
    CHECK(store.list()->size() == 1);
}

TEST_CASE("secret references cannot address other files", "[secrets]") {
    Fixture f;
    FileSecretStore store{f.layout.credentials};
    for (const auto* ref : {"../instance", "sec_../../x", "sec_a/b", "", "cre_123"}) {
        INFO(ref);
        CHECK(store.get(ref).error().code == domain::ErrorCode::validation_failed);
        CHECK_FALSE(store.remove(ref));
    }
}

TEST_CASE("secret size is bounded", "[secrets]") {
    Fixture f;
    FileSecretStore store{f.layout.credentials};
    CHECK_FALSE(store.put(SecretBytes{}));
    CHECK_FALSE(store.put(SecretBytes{std::string(FileSecretStore::max_secret_bytes + 1, 'x')}));
    CHECK(store.put(SecretBytes{std::string(FileSecretStore::max_secret_bytes, 'x')}));
}

TEST_CASE("moved-from secrets are empty", "[secrets]") {
    SecretBytes original{"token"};
    SecretBytes moved{std::move(original)};
    CHECK(moved.reveal() == "token");
    CHECK(original.empty()); // NOLINT(bugprone-use-after-move): verifying the move
}
