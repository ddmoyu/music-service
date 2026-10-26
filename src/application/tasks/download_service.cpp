#include "application/tasks/download_service.hpp"
#include "application/catalog/source_retry.hpp"
#include <nlohmann/json.hpp>
#include "application/media/file_bundle.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <set>
#include <utility>

namespace music_service::application::tasks {

namespace {

using domain::Error;
using domain::ErrorCode;

std::string fnv_hex(std::string_view data) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : data) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[hash & 0xFU];
        hash >>= 4U;
    }
    return out;
}

domain::Result<CreateResult> replay(ports::TaskRepository& tasks, const std::string& owner_id,
                                    const ports::StoredResponse& stored, const std::string& request_hash,
                                    const std::function<domain::Result<TaskView>(ports::TaskRecord)>& view_of);

// The string value of "name": "..." in a stored response, if present.
std::optional<std::string> string_field(const std::string& text, std::string_view name) {
    const auto key = "\"" + std::string{name} + "\":\"";
    const auto at = text.find(key);
    const auto end = at == std::string::npos ? at : text.find('"', at + key.size());
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return text.substr(at + key.size(), end - at - key.size());
}

// The strings of "name": ["...", ...] in a stored response, empty if absent.
std::vector<std::string> string_list(const std::string& text, std::string_view name) {
    std::vector<std::string> out;
    const auto key = "\"" + std::string{name} + "\":[";
    const auto at = text.find(key);
    if (at == std::string::npos) {
        return out;
    }
    const auto close = text.find(']', at + key.size());
    auto open = text.find('"', at + key.size());
    while (open < close) {
        const auto end = text.find('"', open + 1);
        if (end >= close) {
            break;
        }
        out.push_back(text.substr(open + 1, end - open - 1));
        open = text.find('"', end + 1);
    }
    return out;
}

// "lc1.<created_at ms>.<filter digest>.<task_id>": the position after the last item.
std::string make_cursor(const ports::TaskRecord& last, std::string_view filter_digest) {
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(last.created_at.time_since_epoch()).count();
    return "lc1." + std::to_string(millis) + "." + std::string{filter_digest} + "." + last.task_id;
}

std::string filter_digest(const std::string& owner_id, const TaskFilter& filter) {
    std::string text = owner_id + "\x1f";
    text += filter.status ? std::string{domain::to_string(*filter.status)} : "-";
    text += "\x1f" + filter.source.value_or("-") + "\x1f" + filter.batch_id.value_or("-");
    return fnv_hex(text);
}

} // namespace

DownloadService::DownloadService(std::shared_ptr<ports::TaskRepository> tasks,
                                 std::shared_ptr<const ports::ProviderDirectory> providers,
                                 std::shared_ptr<accounts::CredentialService> credentials,
                                 std::vector<DestinationRule> destinations,
                                 std::function<bool(const DestinationRule&)> destination_available,
                                 std::function<bool(const std::string&, const std::string&, std::uint64_t)> file_intact,
                                 std::uint32_t max_batch_items, std::shared_ptr<ports::DownloadQueue> queue,
                                 domain::OutputOptions output, std::shared_ptr<ports::PublishedFiles> bundle_files)
    : tasks_(std::move(tasks)), providers_(std::move(providers)), credentials_(std::move(credentials)),
      destinations_(std::move(destinations)), destination_available_(std::move(destination_available)),
      file_intact_(std::move(file_intact)), max_batch_items_(max_batch_items), queue_(std::move(queue)),
      output_(std::move(output)), bundle_files_(std::move(bundle_files)) {}

void DownloadService::wake() const {
    if (queue_) {
        queue_->wake();
    }
}

domain::Result<void> DownloadService::check_idempotency_key(std::string_view key) {
    const bool printable = std::all_of(key.begin(), key.end(), [](char c) { return c > 0x20 && c < 0x7F; });
    if (key.size() < 16 || key.size() > 128 || !printable) {
        return Error::validation("Idempotency-Key", "must be 16 to 128 printable ASCII characters without spaces");
    }
    return domain::ok();
}

domain::Result<const DestinationRule*>
DownloadService::destination_for(const std::string& owner_id, const std::optional<std::string>& destination_id) const {
    const auto allowed = [&](const DestinationRule& rule) {
        return std::find(rule.principals.begin(), rule.principals.end(), owner_id) != rule.principals.end();
    };
    const DestinationRule* rule = nullptr;
    for (const auto& candidate : destinations_) {
        if (!allowed(candidate)) {
            continue;
        }
        if (!destination_id || candidate.id == *destination_id) {
            rule = &candidate;
            break;
        }
    }
    if (rule == nullptr) {
        // Unknown and not permitted look the same.
        return Error{ErrorCode::not_found, "no such destination", false, {}};
    }
    if (!rule->accept_new_tasks) {
        return Error{ErrorCode::destination_unavailable, "the destination takes no new downloads", false, {}};
    }
    if (destination_available_ && !destination_available_(*rule)) {
        return Error{ErrorCode::destination_unavailable, "the destination is not available", true, {}};
    }
    return rule;
}

domain::Result<domain::NormalizedDownloadIntent> DownloadService::prepare(const std::string& owner_id,
                                                                          const DownloadRequest& request) {
    const auto& source = request.track.source.value();
    if (auto known = providers_->check_source(source); !known) {
        return known.error();
    }
    // Without same-source audio there is no execution path: refuse before accepting.
    if (auto spec = providers_->describe_capability(source, "resolve_audio"); !spec) {
        return spec.error();
    }
    auto binding = credentials_->resolve_binding(owner_id, source, "resolve_audio", request.credential_id);
    if (!binding) {
        return std::move(binding).error();
    }
    catalog::SourceRetryService source_retry{providers_, credentials_};
    auto bindings = source_retry.bindings(owner_id, binding->binding);
    if (!bindings) return bindings.error();
    auto destination = destination_for(owner_id, request.destination_id);
    if (!destination) {
        return std::move(destination).error();
    }
    const auto& rule = *destination.value();
    domain::DownloadIntentDraft draft{owner_id,
                                      request.track,
                                      std::move(bindings).value(),
                                      request.requirement,
                                      request.metadata_policy,
                                      {rule.id, fnv_hex(rule.id + "\x1f" + rule.canonical_root), "naming:" + output_.name_format + ":" + output_.folder_layout,
                                       std::string{"metadata:"} + (output_.embed_cover ? "cover:" : "no-cover:") +
                                       (output_.embed_lyrics ? "lyrics:" : "no-lyrics:") + output_.lyrics_variant, output_}};
    return domain::NormalizedDownloadIntent::normalize(std::move(draft));
}

std::optional<ports::ReuseCandidate> DownloadService::reusable(const domain::NormalizedDownloadIntent& intent) {
    auto results = tasks_->completed_results(intent.owner_id(), domain::fingerprint(intent));
    if (!results) {
        return std::nullopt; // a lookup failure only means no reuse
    }
    for (const auto& result : results.value()) {
        auto members = media::bundle_members(result.properties_json);
        if (!members) continue;
        bool intact = true;
        for (const auto& member : *members) {
            if (!bundle_files_) { intact = false; break; }
            auto checked = bundle_files_->matches(result.destination_id, media::sibling_path(result.relative_path, member.name), member.file);
            if (!checked || !*checked) { intact = false; break; }
        }
        if (!intact) continue;
        // The published file must still be there with its recorded size (A46); the
        // transaction re-checks the versions.
        if (result.file_available && file_intact_ &&
            file_intact_(result.destination_id, result.relative_path, result.size_bytes)) {
            return result.candidate;
        }
    }
    return std::nullopt;
}

domain::Result<TaskView> DownloadService::view_of(ports::TaskRecord task) {
    auto intent = tasks_->get_intent(task.task_id);
    if (!intent) {
        return std::move(intent).error();
    }
    return TaskView{std::move(task), std::move(intent).value()};
}

namespace {

domain::Result<CreateResult> replay(ports::TaskRepository& tasks, const std::string& owner_id,
                                    const ports::StoredResponse& stored, const std::string& request_hash,
                                    const std::function<domain::Result<TaskView>(ports::TaskRecord)>& view_of) {
    if (stored.request_hash != request_hash) {
        return Error{ErrorCode::idempotency_conflict, "Idempotency-Key was used for a different request", false, {}};
    }
    // {"task_id":"...","reused":bool,"status":"..."}
    const auto& text = stored.response_json;
    const auto task_id = string_field(text, "task_id");
    if (!task_id) {
        return Error{ErrorCode::internal, "stored response is invalid", false, {}};
    }
    auto task = tasks.get(owner_id, *task_id);
    if (!task) {
        return std::move(task).error();
    }
    // The first response stands; the current state is read with GET.
    if (const auto status = string_field(text, "status")) {
        if (auto first = domain::parse_task_status(*status); first) {
            task->status = first.value();
        }
    }
    auto view = view_of(std::move(task).value());
    if (!view) {
        return std::move(view).error();
    }
    return CreateResult{std::move(view).value(), text.find("\"reused\":true") != std::string::npos, true,
                        stored.status_code};
}

} // namespace

domain::Result<CreateResult> DownloadService::create(const std::string& owner_id, const DownloadRequest& request,
                                                     const std::string& idempotency_key,
                                                     const std::string& request_hash) {
    if (auto valid = check_idempotency_key(idempotency_key); !valid) {
        return valid.error();
    }
    // A committed request replays before any short-lived state is read again.
    auto stored = tasks_->find_idempotent(owner_id, std::string{create_route}, idempotency_key);
    if (!stored) {
        return std::move(stored).error();
    }
    const auto viewer = [this](ports::TaskRecord task) { return view_of(std::move(task)); };
    if (stored.value()) {
        return replay(*tasks_, owner_id, *stored.value(), request_hash, viewer);
    }
    auto intent = prepare(owner_id, request);
    if (!intent) {
        return std::move(intent).error();
    }
    auto candidate = request.force ? std::nullopt : reusable(intent.value());
    auto outcome = tasks_->create({std::move(intent).value(), idempotency_key, request_hash, request.force,
                                   std::string{create_route}, std::nullopt, std::move(candidate)});
    if (!outcome) {
        return std::move(outcome).error();
    }
    const bool reused = outcome->reused;
    const bool replayed = outcome->replayed;
    if (!reused && !replayed) {
        wake(); // committed: the scheduler may claim it now
    }
    auto view = view_of(std::move(outcome->task));
    if (!view) {
        return std::move(view).error();
    }
    return CreateResult{std::move(view).value(), reused, replayed, reused ? 200 : 202};
}

domain::Result<BatchResult> DownloadService::create_batch(const std::string& owner_id,
                                                          const std::vector<DownloadRequest>& items,
                                                          const std::string& idempotency_key,
                                                          const std::string& request_hash) {
    if (auto valid = check_idempotency_key(idempotency_key); !valid) {
        return valid.error();
    }
    const auto to_result = [this](ports::BatchOutcome outcome) -> domain::Result<BatchResult> {
        BatchResult result;
        result.batch_id = outcome.batch.batch_id;
        result.created_count = outcome.batch.created_count;
        result.reused_count = outcome.batch.reused_count;
        result.reused = std::move(outcome.reused);
        result.replayed = outcome.replayed;
        result.status_code = result.created_count > 0 ? 202 : 200;
        for (auto& task : outcome.batch.items) {
            auto view = view_of(std::move(task));
            if (!view) {
                return std::move(view).error();
            }
            result.items.push_back(std::move(view).value());
        }
        return result;
    };

    auto stored = tasks_->find_idempotent(owner_id, std::string{batch_route}, idempotency_key);
    if (!stored) {
        return std::move(stored).error();
    }
    if (stored.value() && stored.value()->request_hash != request_hash) {
        return Error{ErrorCode::idempotency_conflict, "Idempotency-Key was used for a different request", false, {}};
    }

    if (stored.value()) {
        // The first outcome stands, even if today's checks or limits would refuse it:
        // {"batch_id":"...","reused":[true,false,...],"status":["queued",...]}.
        const auto& text = stored.value()->response_json;
        const auto batch_id = string_field(text, "batch_id");
        const auto list_at = text.find("\"reused\":[");
        if (!batch_id || list_at == std::string::npos) {
            return Error{ErrorCode::internal, "stored response is invalid", false, {}};
        }
        auto batch = tasks_->get_batch(owner_id, *batch_id);
        if (!batch) {
            return std::move(batch).error();
        }
        std::vector<bool> reused;
        for (auto i = text.find_first_of("tf", list_at + 10); i != std::string::npos && i < text.find(']', list_at);
             i = text.find_first_of("tf", i + 4)) {
            reused.push_back(text[i] == 't');
        }
        // Records written before the statuses were kept show the current ones.
        if (const auto statuses = string_list(text, "status"); statuses.size() == batch->items.size()) {
            for (std::size_t i = 0; i < statuses.size(); ++i) {
                if (auto first = domain::parse_task_status(statuses[i]); first) {
                    batch->items[i].status = first.value();
                }
            }
        }
        return to_result(ports::BatchOutcome{std::move(batch).value(), std::move(reused), true});
    }

    // Only a new request is held to today's batch limit.
    if (items.empty() || items.size() > max_batch_items_) {
        return Error::validation("items", "must hold 1 to " + std::to_string(max_batch_items_) + " downloads");
    }

    // All items are checked; the first failure decides the status, all are listed.
    std::vector<ports::BatchItemCommand> commands;
    std::optional<Error> first_error;
    std::vector<domain::ErrorDetail> failures;
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto intent = prepare(owner_id, items[i]);
        if (!intent) {
            const auto& error = intent.error();
            failures.push_back({"items[" + std::to_string(i) + "]",
                                std::string{domain::to_string(error.code)} + ": " + error.message});
            if (!first_error) {
                first_error = error;
            }
            continue;
        }
        auto candidate = items[i].force ? std::nullopt : reusable(intent.value());
        commands.push_back({std::move(intent).value(), items[i].force, std::move(candidate)});
    }
    if (first_error) {
        first_error->details = std::move(failures);
        return *first_error;
    }
    auto outcome = tasks_->create_batch({owner_id, idempotency_key, request_hash, std::move(commands)});
    if (!outcome) {
        return std::move(outcome).error();
    }
    if (!outcome->replayed) {
        wake();
    }
    return to_result(std::move(outcome).value());
}

domain::Result<CreateResult> DownloadService::retry(const std::string& owner_id, const std::string& task_id,
                                                    const std::string& idempotency_key,
                                                    const std::string& request_hash) {
    if (auto valid = check_idempotency_key(idempotency_key); !valid) {
        return valid.error();
    }
    const auto route = std::string{create_route} + "/" + task_id + "/retry";
    auto stored = tasks_->find_idempotent(owner_id, route, idempotency_key);
    if (!stored) {
        return std::move(stored).error();
    }
    const auto viewer = [this](ports::TaskRecord task) { return view_of(std::move(task)); };
    if (stored.value()) {
        return replay(*tasks_, owner_id, *stored.value(), request_hash, viewer);
    }
    auto original = tasks_->get(owner_id, task_id);
    if (!original) {
        return std::move(original).error();
    }
    if (original->status != domain::TaskStatus::failed && original->status != domain::TaskStatus::canceled) {
        return Error{ErrorCode::conflict, "only failed or canceled tasks can be retried", false, {}};
    }
    auto intent = tasks_->get_intent(task_id);
    if (!intent) {
        return std::move(intent).error();
    }
    // 手动重试沿用固定账号和严格身份，跳过已经尝试的平台；匹配在后台完成。
    if (auto destination = destination_for(owner_id, intent->product().destination_id); !destination) {
        return std::move(destination).error();
    }
    auto state = catalog::decode_source_retry(original->execution_json, intent->original_track());
    if (!state) return state.error();
    const auto& source = intent->original_track().source.value();
    if (std::find(state->attempted_sources.begin(), state->attempted_sources.end(), source) == state->attempted_sources.end())
        state->attempted_sources.push_back(source);
    state->pending = true;
    const auto seed = nlohmann::json{{"source_retry", nlohmann::json::parse(catalog::encode_source_retry(*state))}}.dump();
    auto outcome = tasks_->create({std::move(intent).value(), idempotency_key, request_hash, false, route, task_id,
                                   std::nullopt, seed});
    if (!outcome) {
        return std::move(outcome).error();
    }
    if (!outcome->replayed) {
        wake();
    }
    auto view = view_of(std::move(outcome->task));
    if (!view) {
        return std::move(view).error();
    }
    return CreateResult{std::move(view).value(), false, outcome->replayed, 202};
}

domain::Result<TaskView> DownloadService::get(const std::string& owner_id, const std::string& task_id) {
    auto task = tasks_->get(owner_id, task_id);
    if (!task) {
        return std::move(task).error();
    }
    return view_of(std::move(task).value());
}

domain::Result<TaskPage> DownloadService::list(const std::string& owner_id, const TaskFilter& filter) {
    if (filter.limit < 1 || filter.limit > max_page) {
        return Error::validation("limit", "must be between 1 and 100");
    }
    const auto digest = filter_digest(owner_id, filter);
    ports::TaskQuery query{owner_id, filter.status, filter.source, filter.batch_id, filter.limit + 1, std::nullopt};
    if (filter.cursor) {
        // lc1.<ms>.<digest>.<task_id>
        const auto& c = *filter.cursor;
        const auto a = c.find('.');
        const auto b = a == std::string::npos ? a : c.find('.', a + 1);
        const auto d = b == std::string::npos ? b : c.find('.', b + 1);
        std::int64_t millis = 0;
        const bool parsed = a != std::string::npos && b != std::string::npos && d != std::string::npos &&
                            c.substr(0, a) == "lc1" &&
                            std::from_chars(c.data() + a + 1, c.data() + b, millis).ec == std::errc{};
        if (!parsed) {
            return Error{ErrorCode::cursor_mismatch, "not a download list cursor", false, {}};
        }
        if (c.substr(b + 1, d - b - 1) != digest) {
            return Error{ErrorCode::cursor_mismatch, "the cursor belongs to other filters", false, {}};
        }
        query.after = std::pair{std::chrono::system_clock::time_point{std::chrono::milliseconds{millis}},
                                c.substr(d + 1)};
    }
    auto found = tasks_->list(query);
    if (!found) {
        return std::move(found).error();
    }
    auto& rows = found.value();
    TaskPage page;
    if (rows.size() > filter.limit) {
        rows.resize(filter.limit);
        page.next_cursor = make_cursor(rows.back(), digest);
    }
    for (auto& row : rows) {
        auto view = view_of(std::move(row));
        if (!view) {
            return std::move(view).error();
        }
        page.items.push_back(std::move(view).value());
    }
    return page;
}

domain::Result<CancelResult> DownloadService::cancel(const std::string& owner_id, const std::string& task_id) {
    auto before = tasks_->get(owner_id, task_id);
    if (!before) {
        return std::move(before).error();
    }
    auto after = tasks_->request_cancel(owner_id, task_id);
    if (!after) {
        return std::move(after).error();
    }
    const bool asynchronous = after->status == domain::TaskStatus::canceling;
    if (asynchronous && queue_) {
        queue_->request_stop(task_id); // committed first; the runner finishes the cancel
    }
    auto view = view_of(std::move(after).value());
    if (!view) {
        return std::move(view).error();
    }
    return CancelResult{std::move(view).value(), asynchronous};
}

domain::Result<DownloadService::BatchView> DownloadService::get_batch(const std::string& owner_id,
                                                                      const std::string& batch_id) {
    auto batch = tasks_->get_batch(owner_id, batch_id);
    if (!batch) {
        return std::move(batch).error();
    }
    BatchView view{std::move(batch).value(), {}};
    for (const auto& task : view.batch.items) {
        auto item = view_of(task);
        if (!item) {
            return std::move(item).error();
        }
        view.items.push_back(std::move(item).value());
    }
    return view;
}

domain::Result<BatchCancelResult> DownloadService::cancel_batch(const std::string& owner_id,
                                                                const std::string& batch_id) {
    auto batch = tasks_->get_batch(owner_id, batch_id);
    if (!batch) {
        return std::move(batch).error();
    }
    BatchCancelResult result;
    std::set<std::string> seen;
    for (const auto& task : batch->items) {
        if (!seen.insert(task.task_id).second) {
            continue; // one cancel per task, however often the batch lists it
        }
        auto canceled = cancel(owner_id, task.task_id);
        if (canceled) {
            result.asynchronous = result.asynchronous || canceled->asynchronous;
            result.accepted.push_back(std::move(canceled->view));
            continue;
        }
        auto current = get(owner_id, task.task_id);
        if (!current) {
            return std::move(current).error();
        }
        result.refused.push_back({std::move(current).value(), std::move(canceled).error()});
    }
    return result;
}

} // namespace music_service::application::tasks
