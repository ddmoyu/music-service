#include "test_support.hpp"

#include "music_dl/domain/identity/account.hpp"

using namespace music_dl::domain;
using music_dl::test::anonymous;
using music_dl::test::source;
using music_dl::test::unwrap;
using music_dl::test::user;

TEST_CASE("account bindings enforce the mode rules", "[domain][identity]") {
    const auto kuwo = source("kuwo");

    CHECK(AccountBinding::make("prn_a", kuwo, AccountMode::anonymous, "anonymous", std::nullopt,
                               std::nullopt));
    CHECK_FALSE(AccountBinding::make("prn_a", kuwo, AccountMode::anonymous, "", std::nullopt,
                                     std::nullopt));
    CHECK_FALSE(AccountBinding::make("prn_a", kuwo, AccountMode::anonymous, "anonymous",
                                     std::string{"cre_1"}, std::nullopt));

    CHECK(AccountBinding::make("prn_a", kuwo, AccountMode::user, "cookie", std::string{"cre_1"},
                               std::nullopt));
    CHECK_FALSE(AccountBinding::make("prn_a", kuwo, AccountMode::user, "cookie", std::nullopt,
                                     std::nullopt));
    CHECK_FALSE(AccountBinding::make("prn_a", kuwo, AccountMode::user, "cookie",
                                     std::string{"not-a-credential"}, std::nullopt));

    CHECK(AccountBinding::make("prn_a", kuwo, AccountMode::application, "app",
                               std::nullopt, std::string{"spotify_app"}));
    CHECK_FALSE(AccountBinding::make("prn_a", kuwo, AccountMode::application, "app",
                                     std::string{"cre_1"}, std::string{"spotify_app"}));

    CHECK_FALSE(AccountBinding::make("alice", kuwo, AccountMode::anonymous, "anonymous",
                                     std::nullopt, std::nullopt));
}

TEST_CASE("versions must match the references a binding uses", "[domain][identity]") {
    const auto binding = user("prn_a", "qq", "cre_1");

    CHECK(AuthIdentityKey::make(binding, {1, std::nullopt, std::nullopt, std::nullopt}));
    CHECK_FALSE(AuthIdentityKey::make(binding, {}));
    CHECK_FALSE(AuthIdentityKey::make(binding, {1, 1, std::nullopt, std::nullopt}));
    CHECK_FALSE(AuthIdentityKey::make(anonymous("prn_a", "qq"),
                                      {std::nullopt, std::nullopt, std::string{"tok"}, 1}));
}

// A38: two credentials of one principal and source, both at version 1, must not share
// caches, cursors or connections.
TEST_CASE("equal versions of different credentials are different identities",
          "[domain][identity]") {
    const auto first = unwrap(
        AuthIdentityKey::make(user("prn_a", "qq", "cre_1"), {1, std::nullopt, std::nullopt,
                                                             std::nullopt}));
    const auto second = unwrap(
        AuthIdentityKey::make(user("prn_a", "qq", "cre_2"), {1, std::nullopt, std::nullopt,
                                                             std::nullopt}));
    const auto first_again = unwrap(
        AuthIdentityKey::make(user("prn_a", "qq", "cre_1"), {1, std::nullopt, std::nullopt,
                                                             std::nullopt}));
    const auto first_v2 = unwrap(
        AuthIdentityKey::make(user("prn_a", "qq", "cre_1"), {2, std::nullopt, std::nullopt,
                                                             std::nullopt}));

    CHECK_FALSE(first == second);
    CHECK(first.canonical() != second.canonical());
    CHECK(first == first_again);
    CHECK(first.canonical() == first_again.canonical());
    CHECK_FALSE(first == first_v2);
    CHECK(first.binding() == first_v2.binding()); // stable selection survives a refresh
}

TEST_CASE("identities of different principals never collide", "[domain][identity]") {
    const auto a = unwrap(AuthIdentityKey::make(anonymous("prn_a", "kuwo"), {}));
    const auto b = unwrap(AuthIdentityKey::make(anonymous("prn_b", "kuwo"), {}));
    CHECK_FALSE(a == b);
    CHECK(a.canonical() != b.canonical());
}
