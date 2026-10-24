#include "test_support.hpp"

#include "domain/tasks/download_intent.hpp"
#include "infrastructure/storage/intent_codec.hpp"
#include <nlohmann/json.hpp>

using namespace music_service::domain;
using music_service::test::anonymous;
using music_service::test::source;
using music_service::test::track;
using music_service::test::unwrap;
using music_service::test::user;

namespace {

DownloadIntentDraft base_draft() {
    DownloadIntentDraft draft{
        "prn_a",
        track("kuwo", "MUSIC_1"),
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

TEST_CASE("current output settings survive storage and isolate reuse", "[domain][intent][output]") {
    namespace storage = music_service::infrastructure::storage;
    auto draft = base_draft();
    draft.product.output = OutputOptions{};
    const auto base = fp(draft);
    const auto saved = unwrap(NormalizedDownloadIntent::normalize(draft));
    const auto decoded = unwrap(storage::decode_intent(storage::encode_intent(saved)));
    CHECK(decoded.product().output == draft.product.output);
    CHECK(fingerprint(decoded) == base);
    SECTION("name") { draft.product.output.name_format = "title"; }
    SECTION("folders") { draft.product.output.folder_layout = "artist_album"; }
    SECTION("cover") { draft.product.output.embed_cover = false; }
    SECTION("lyrics") { draft.product.output.embed_lyrics = false; }
    SECTION("sidecars") {
        draft.product.output.export_lyrics = true;
        draft.product.output.export_cover = true;
        draft.product.output.sidecars_required = true;
        draft.product.output.lyrics_variant = "romanized";
        auto selected = unwrap(NormalizedDownloadIntent::normalize(draft));
        const auto stored = storage::encode_intent(selected);
        auto restored = unwrap(storage::decode_intent(stored));
        CHECK(restored.product().output == draft.product.output);
        CHECK(fingerprint(restored) == fingerprint(selected));
        auto optional = draft;
        optional.product.output.sidecars_required = false;
        CHECK_FALSE(fp(optional) == fp(draft));
    }
    SECTION("size") { draft.product.output.cover_max_bytes = 1024; }
    SECTION("budget") { draft.product.output.metadata_timeout_seconds = 2; }
    SECTION("lyrics variant") {
        draft.product.output.lyrics_variant = "verbatim";
        auto selected = unwrap(NormalizedDownloadIntent::normalize(draft));
        const auto stored = storage::encode_intent(selected);
        auto restored = unwrap(storage::decode_intent(stored));
        CHECK(restored.product().output.lyrics_variant == "verbatim");
        CHECK(fingerprint(restored) == fingerprint(selected));
    }
    CHECK_FALSE(fp(draft) == base);
    CHECK(decoded.product().output == OutputOptions{});
}

TEST_CASE("stored cross-source selections fail without becoming same-source downloads", "[domain][intent]") {
    namespace storage = music_service::infrastructure::storage;
    const auto intent = unwrap(NormalizedDownloadIntent::normalize(base_draft()));
    auto document = nlohmann::json::parse(storage::encode_intent(intent));
    for (const auto kind : {"same", "manual", "auto"}) {
        document["selection"] = {{"kind", kind}, {"target", {{"source", "qq"}, {"id", "different"}}}};
        const auto restored = storage::decode_intent(document.dump());
        REQUIRE_FALSE(restored);
        CHECK(restored.error().message.find("current schema") != std::string::npos);
    }
}

TEST_CASE("non-current and incomplete snapshots are rejected", "[domain][intent]") {
    namespace storage = music_service::infrastructure::storage;
    const auto intent = unwrap(NormalizedDownloadIntent::normalize(base_draft()));
    const auto current = nlohmann::json::parse(storage::encode_intent(intent));
    for (const auto version : {1, 2, 3, 4, 6}) {
        auto document = current;
        document["schema"] = version;
        CHECK_FALSE(storage::decode_intent(document.dump()));
    }
    for (const auto& [field, value] : current.at("product").at("output").items()) {
        auto document = current;
        document["product"]["output"].erase(field);
        CHECK_FALSE(storage::decode_intent(document.dump()));
    }
    auto document = current;
    document["product"].erase("output");
    CHECK_FALSE(storage::decode_intent(document.dump()));
}

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
