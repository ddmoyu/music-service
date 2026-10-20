#include "test_support.hpp"

#include "music_dl/domain/tasks/download_intent.hpp"

using namespace music_dl::domain;
using music_dl::test::anonymous;
using music_dl::test::source;
using music_dl::test::track;
using music_dl::test::unwrap;
using music_dl::test::user;

namespace {

DownloadIntentDraft base_draft() {
    DownloadIntentDraft draft{
        "prn_a",
        track("kuwo", "MUSIC_1"),
        SameSource{},
        {anonymous("prn_a", "kuwo")},
        {},
        MetadataPolicy::best_effort,
        {"dst_default", "d1", "n1", "m1"},
    };
    return draft;
}

DownloadFingerprint fp(DownloadIntentDraft draft) {
    return fingerprint(unwrap(NormalizedDownloadIntent::normalize(std::move(draft))));
}

} // namespace

TEST_CASE("the same intent produces the same fingerprint", "[domain][intent]") {
    const auto a = fp(base_draft());
    const auto b = fp(base_draft());
    CHECK(a == b);
    CHECK(a.digest == b.digest);
    CHECK(a.digest.size() == 16);
    CHECK(a.schema_version == DownloadFingerprint::current_schema);
}

// A35: different preview or metadata requirements must not reuse each other.
TEST_CASE("preview and metadata requirements are part of the intent", "[domain][intent]") {
    const auto base = fp(base_draft());

    auto preview = base_draft();
    preview.requirement.allow_preview = true;
    CHECK_FALSE(fp(preview) == base);

    auto required = base_draft();
    required.metadata_policy = MetadataPolicy::required;
    auto skip = base_draft();
    skip.metadata_policy = MetadataPolicy::skip;
    CHECK_FALSE(fp(required) == base);
    CHECK_FALSE(fp(required) == fp(skip));
}

TEST_CASE("quality, destination and product configuration are part of the intent",
          "[domain][intent]") {
    const auto base = fp(base_draft());

    auto high = base_draft();
    high.requirement.requested = Quality::high;
    CHECK_FALSE(fp(high) == base);

    auto lower = base_draft();
    lower.requirement.policy = QualityPolicy::allow_lower;
    CHECK_FALSE(fp(lower) == base);

    auto destination = base_draft();
    destination.product.destination_id = "dst_other";
    CHECK_FALSE(fp(destination) == base);

    auto naming = base_draft();
    naming.product.naming_config_version = "n2";
    CHECK_FALSE(fp(naming) == base);
}

// A36: different manual targets differ; the same target reached through different
// short-lived match handles is the same intent because handles are never stored.
TEST_CASE("manual targets are stored by their verified track", "[domain][intent]") {
    auto to_qq_1 = base_draft();
    to_qq_1.selection = ManualTarget{track("qq", "song_1")};
    to_qq_1.account_bindings.push_back(user("prn_a", "qq", "cre_1"));

    auto to_qq_1_again = to_qq_1;
    auto to_qq_2 = to_qq_1;
    to_qq_2.selection = ManualTarget{track("qq", "song_2")};

    CHECK(fp(to_qq_1) == fp(to_qq_1_again));
    CHECK_FALSE(fp(to_qq_1) == fp(to_qq_2));
    CHECK_FALSE(fp(to_qq_1) == fp(base_draft()));
}

TEST_CASE("a manual target equal to the original is the same source", "[domain][intent]") {
    auto manual_self = base_draft();
    manual_self.selection = ManualTarget{track("kuwo", "MUSIC_1")};
    const auto intent = unwrap(NormalizedDownloadIntent::normalize(manual_self));
    CHECK(std::holds_alternative<SameSource>(intent.selection()));
    CHECK(fingerprint(intent) == fp(base_draft()));
}

TEST_CASE("account bindings are fixed and part of the intent", "[domain][intent]") {
    auto with_user = base_draft();
    with_user.account_bindings = {user("prn_a", "kuwo", "cre_1")};
    auto with_other_user = base_draft();
    with_other_user.account_bindings = {user("prn_a", "kuwo", "cre_2")};

    CHECK_FALSE(fp(with_user) == fp(base_draft()));
    CHECK_FALSE(fp(with_user) == fp(with_other_user));
}

TEST_CASE("bindings for unused sources are dropped and order does not matter",
          "[domain][intent]") {
    auto extra = base_draft();
    extra.account_bindings.insert(extra.account_bindings.begin(), anonymous("prn_a", "qq"));
    const auto intent = unwrap(NormalizedDownloadIntent::normalize(extra));
    REQUIRE(intent.account_bindings().size() == 1);
    CHECK(intent.account_bindings()[0].source() == source("kuwo"));
    CHECK(fingerprint(intent) == fp(base_draft()));
}

TEST_CASE("binding errors are rejected", "[domain][intent]") {
    auto missing = base_draft();
    missing.account_bindings.clear();
    CHECK_FALSE(NormalizedDownloadIntent::normalize(missing));

    auto foreign = base_draft();
    foreign.account_bindings = {anonymous("prn_b", "kuwo")};
    CHECK_FALSE(NormalizedDownloadIntent::normalize(foreign));

    auto conflicting = base_draft();
    conflicting.account_bindings.push_back(user("prn_a", "kuwo", "cre_1"));
    CHECK_FALSE(NormalizedDownloadIntent::normalize(conflicting));

    auto duplicate = base_draft();
    duplicate.account_bindings.push_back(anonymous("prn_a", "kuwo"));
    CHECK(NormalizedDownloadIntent::normalize(duplicate));
}

TEST_CASE("auto fallback keeps candidate order and removes duplicates", "[domain][intent]") {
    auto draft = base_draft();
    draft.account_bindings.push_back(anonymous("prn_a", "qq"));
    draft.account_bindings.push_back(anonymous("prn_a", "migu"));

    auto qq_first = draft;
    qq_first.selection = AutoFallback{{source("qq"), source("kuwo"), source("migu"), source("qq")}, 1};
    auto qq_first_clean = draft;
    qq_first_clean.selection = AutoFallback{{source("qq"), source("migu")}, 1};
    auto migu_first = draft;
    migu_first.selection = AutoFallback{{source("migu"), source("qq")}, 1};
    auto newer_strategy = draft;
    newer_strategy.selection = AutoFallback{{source("qq"), source("migu")}, 2};

    const auto intent = unwrap(NormalizedDownloadIntent::normalize(qq_first));
    const auto& fallback = std::get<AutoFallback>(intent.selection());
    REQUIRE(fallback.candidate_sources.size() == 2);
    CHECK(fallback.candidate_sources[0] == source("qq"));
    CHECK(fallback.candidate_sources[1] == source("migu"));

    CHECK(fp(qq_first) == fp(qq_first_clean));
    CHECK_FALSE(fp(qq_first) == fp(migu_first));
    CHECK_FALSE(fp(qq_first) == fp(newer_strategy));
}

TEST_CASE("auto fallback needs a real candidate and a strategy version", "[domain][intent]") {
    auto only_original = base_draft();
    only_original.selection = AutoFallback{{source("kuwo")}, 1};
    CHECK_FALSE(NormalizedDownloadIntent::normalize(only_original));

    auto no_strategy = base_draft();
    no_strategy.account_bindings.push_back(anonymous("prn_a", "qq"));
    no_strategy.selection = AutoFallback{{source("qq")}, 0};
    CHECK_FALSE(NormalizedDownloadIntent::normalize(no_strategy));

    auto no_binding = base_draft();
    no_binding.selection = AutoFallback{{source("qq")}, 1};
    CHECK_FALSE(NormalizedDownloadIntent::normalize(no_binding));
}

TEST_CASE("invalid owners and product settings are rejected", "[domain][intent]") {
    auto owner = base_draft();
    owner.owner_id = "alice";
    CHECK_FALSE(NormalizedDownloadIntent::normalize(owner));

    auto destination = base_draft();
    destination.product.destination_id = "/home/music";
    CHECK_FALSE(NormalizedDownloadIntent::normalize(destination));

    auto version = base_draft();
    version.product.metadata_config_version.clear();
    CHECK_FALSE(NormalizedDownloadIntent::normalize(version));
}

TEST_CASE("platform ids containing separators cannot collide", "[domain][intent]") {
    auto a = base_draft();
    a.original_track = track("kuwo", "a;b");
    auto b = base_draft();
    b.original_track = track("kuwo", "a");
    CHECK_FALSE(fp(a) == fp(b));
}
