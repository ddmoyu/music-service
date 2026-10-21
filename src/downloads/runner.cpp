#include "downloads/runner.hpp"

#include "application/transport/http_rules.hpp"
#include "application/transport/upstream_client.hpp"
#include "downloads/resume_policy.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <future>
#include <random>
#include <thread>
#include <utility>
#include <variant>

namespace music_service::downloads {

namespace {

using domain::Error;
using domain::ErrorCode;
using domain::TaskStatus;
using nlohmann::json;
using Steady = std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

constexpr std::string_view work_file = "input.part";

std::string error_json(const Error& error) {
    json details = json::array();
    for (const auto& detail : error.details) {
        details.push_back({{"field", detail.field}, {"reason", detail.reason}});
    }
    return json{{"code", std::string{domain::to_string(error.code)}},
                {"message", error.message},
                {"retryable", error.retryable},
                {"details", std::move(details)}}
        .dump();
}

Error error(ErrorCode code, std::string message, bool retryable, std::string field = {}, std::string reason = {}) {
    Error out{code, std::move(message), retryable, {}};
    if (!field.empty()) {
        out.details.push_back({std::move(field), std::move(reason)});
    }
    return out;
}

// A request that could not be admitted now, as provider packages report it.
bool admission_deferred(const Error& e) {
    return e.code == ErrorCode::service_busy &&
           std::any_of(e.details.begin(), e.details.end(), [](const auto& d) { return d.field == "admission"; });
}

// The upstream's own wait, as the error carries it (retry_after_seconds).
std::optional<std::chrono::seconds> retry_hint(const Error& e) {
    for (const auto& detail : e.details) {
        if (detail.field == "retry_after_seconds") {
            return application::transport::parse_retry_after(detail.reason);
        }
    }
    return std::nullopt;
}

bool storage_busy(const Error& e) {
    return e.code == ErrorCode::storage_unavailable || e.code == ErrorCode::service_busy;
}

std::optional<std::uint64_t> parse_uint(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// "bytes 100-199/2000"; the total may be "*".
struct ContentRange {
    std::uint64_t start{0};
    std::optional<std::uint64_t> total;
};

std::optional<ContentRange> parse_content_range(std::string_view value) {
    if (value.substr(0, 6) != "bytes ") {
        return std::nullopt;
    }
    value.remove_prefix(6);
    const auto dash = value.find('-');
    const auto slash = value.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || dash > slash) {
        return std::nullopt;
    }
    const auto start = parse_uint(value.substr(0, dash));
    if (!start || !parse_uint(value.substr(dash + 1, slash - dash - 1))) {
        return std::nullopt;
    }
    const auto total_text = value.substr(slash + 1);
    return ContentRange{*start, total_text == "*" ? std::nullopt : parse_uint(total_text)};
}

json ref_json(const domain::TrackRef& ref) {
    return json{{"source", ref.source.value()}, {"id", ref.id.value()}};
}

std::string track_key(const domain::TrackRef& ref) {
    return ref.source.value() + ":" + ref.id.value();
}

template <class T> json nullable(const std::optional<T>& value) {
    return value ? json(*value) : json(nullptr);
}

// Private resume evidence (download_tasks.checkpoint). durable_bytes were flushed before
// the checkpoint that names them was committed.
struct Checkpoint {
    std::string track;
    std::string quality;
    std::string name; // the file name chosen for publication, without the extension
    std::uint64_t durable_bytes{0};
    std::optional<std::uint64_t> total_bytes;
    std::optional<std::string> etag;
    std::optional<std::string> last_modified;
    bool complete{false}; // the whole resource is in the work file

    [[nodiscard]] std::string encode() const {
        return json{{"file", work_file},
                    {"track", track},
                    {"quality", quality},
                    {"name", name},
                    {"durable_bytes", durable_bytes},
                    {"total_bytes", nullable(total_bytes)},
                    {"etag", nullable(etag)},
                    {"last_modified", nullable(last_modified)},
                    {"complete", complete}}
            .dump();
    }

    static std::optional<Checkpoint> decode(const std::optional<std::string>& text) {
        if (!text) {
            return std::nullopt;
        }
        const auto j = json::parse(*text, nullptr, false);
        if (!j.is_object() || j.value("file", "") != work_file) {
            return std::nullopt;
        }
        Checkpoint c;
        try {
            c.track = j.at("track").get<std::string>();
            c.quality = j.at("quality").get<std::string>();
            c.name = j.value("name", "");
            c.durable_bytes = j.at("durable_bytes").get<std::uint64_t>();
            if (j.contains("total_bytes") && j["total_bytes"].is_number_unsigned()) {
                c.total_bytes = j["total_bytes"].get<std::uint64_t>();
            }
            if (j.contains("etag") && j["etag"].is_string()) {
                c.etag = j["etag"].get<std::string>();
            }
            if (j.contains("last_modified") && j["last_modified"].is_string()) {
                c.last_modified = j["last_modified"].get<std::string>();
            }
            c.complete = j.value("complete", false);
        } catch (const json::exception&) {
            return std::nullopt;
        }
        return c;
    }
};

// Public transfer progress (data-models.md, 进度): unknown values stay null.
struct Progress {
    std::uint64_t received{0};
    std::optional<std::uint64_t> total;
    std::optional<std::uint64_t> speed; // bytes per second over the last interval

    [[nodiscard]] std::string encode() const {
        json eta = nullptr;
        json stage = nullptr;
        if (total && *total > 0) {
            stage = std::min(1.0, static_cast<double>(received) / static_cast<double>(*total));
            if (speed && *speed > 0 && *total >= received) {
                eta = (*total - received) * 1000 / *speed;
            }
        }
        return json{{"received_bytes", received},
                    {"total_bytes", nullable(total)},
                    {"speed_bytes_per_second", nullable(speed)},
                    {"eta_ms", std::move(eta)},
                    {"stage_progress", std::move(stage)}}
            .dump();
    }
};

class Execution {
public:
    Execution(const RunnerOptions& options, const RunnerServices& services, ports::TaskRecord task,
              ExecutionControl& control)
        : o_(options), s_(services), task_(std::move(task)), control_(control),
          execution_id_(task_.execution_id.value_or("")) {}

    RunReport run();

private:
    // ---- outcomes
    RunReport fail(const Error& cause);
    RunReport retry_later(const Error& cause, bool discard_partial = false);
    RunReport pause(TaskStatus resume, std::chrono::system_clock::time_point when);
    RunReport stopped(TaskStatus resume);
    RunReport after_conflict();
    RunReport finish_cancel();
    RunReport expired();
    RunReport on_error(const Error& e, TaskStatus resume);
    RunReport storage_lost(const Error& e);

    // ---- stages
    std::optional<RunReport> prepare();
    std::optional<RunReport> resolve();
    std::optional<RunReport> choose_name();
    std::optional<RunReport> enter_downloading();
    std::optional<RunReport> download();
    std::optional<RunReport> process();
    RunReport publish();

    // ---- helpers
    template <class Write> domain::Result<ports::TaskRecord> write(Write&& call);
    std::optional<RunReport> adopt(domain::Result<ports::TaskRecord> written);
    [[nodiscard]] bool stop_requested() const { return control_.stop.stop_requested(); }
    [[nodiscard]] milliseconds attempt_used() const {
        return used_before_ + duration_cast<milliseconds>(s_.clock->steady_now() - attempt_started_);
    }
    [[nodiscard]] TaskStatus resume_stage() const {
        return task_.status == TaskStatus::downloading || task_.status == TaskStatus::processing
                   ? TaskStatus::downloading
                   : TaskStatus::resolving;
    }
    [[nodiscard]] ports::OperationContext context() const { return {deadline_, control_.stop.get_token()}; }
    [[nodiscard]] std::string execution_json() const;
    bool wait(milliseconds delay) const;
    domain::Result<ports::TaskRecord> write_progress();
    std::optional<RunReport> save_checkpoint(bool flush);

    const RunnerOptions& o_;
    const RunnerServices& s_;
    ports::TaskRecord task_;
    ExecutionControl& control_;
    std::string execution_id_;

    std::optional<domain::NormalizedDownloadIntent> intent_;
    std::optional<domain::TrackRef> actual_; // set by prepare()
    std::optional<application::accounts::CredentialLease> lease_;
    std::shared_ptr<const ports::SecretBytes> secret_;

    Steady::time_point attempt_started_{};
    milliseconds used_before_{0};
    Steady::time_point deadline_{};
    bool task_deadline_first_{false}; // the absolute deadline ends this execution first

    std::optional<ports::ResolvedAudio> audio_;
    std::vector<domain::ResourceWarning> warnings_;
    std::optional<Checkpoint> checkpoint_;
    std::string name_;
    std::unique_ptr<ports::WorkFile> file_;
    Progress progress_;
    std::optional<ports::ProcessingReport> report_;
};

template <class Write> domain::Result<ports::TaskRecord> Execution::write(Write&& call) {
    // A busy database is retried briefly; the execution keeps the task meanwhile, so no
    // other runner can claim it (runtime-and-tasks.md, 入队与调度).
    auto result = call();
    for (std::uint32_t i = 0; !result && storage_busy(result.error()) && i < o_.storage_retries; ++i) {
        std::this_thread::sleep_for(milliseconds{100} * (i + 1));
        result = call();
    }
    return result;
}

// Keeps the committed record; a refused or failed write ends the execution.
std::optional<RunReport> Execution::adopt(domain::Result<ports::TaskRecord> written) {
    if (written) {
        task_ = std::move(written).value();
        return std::nullopt;
    }
    if (written.error().code == ErrorCode::version_conflict) {
        return after_conflict();
    }
    return storage_lost(written.error());
}

// The database refused a write the execution cannot do without. The task stays in its
// running stage; startup recovery requeues it at the next start.
RunReport Execution::storage_lost(const Error& e) {
    return RunReport{RunOutcome::left, std::string{domain::to_string(e.code)}};
}

std::string Execution::execution_json() const {
    json warnings = json::array();
    for (const auto w : warnings_) {
        warnings.push_back(std::string{domain::to_string(w)});
    }
    json out{{"actual_track", ref_json(*actual_)}, {"warnings", std::move(warnings)}};
    if (audio_) {
        out["actual_quality"] =
            audio_->actual_quality ? json(std::string{domain::to_string(*audio_->actual_quality)}) : json(nullptr);
        out["container"] = nullable(audio_->container);
        out["bitrate_bps"] = nullable(audio_->bitrate_bps);
        out["completeness"] = std::string{domain::to_string(audio_->completeness)};
    }
    if (report_) {
        out["container"] = report_->facts.container;
        out["completeness"] = std::string{domain::to_string(report_->completeness)};
        out["processing_warnings"] = report_->warnings;
    }
    return out.dump();
}

bool Execution::wait(milliseconds delay) const {
    const auto until = std::min(s_.clock->steady_now() + delay, deadline_);
    while (s_.clock->steady_now() < until) {
        if (stop_requested()) {
            return false;
        }
        std::this_thread::sleep_for(
            std::min(milliseconds{50}, duration_cast<milliseconds>(until - s_.clock->steady_now())));
    }
    return !stop_requested();
}

RunReport Execution::fail(const Error& cause) {
    auto done = write([&] {
        return s_.tasks->transition(
            {task_.task_id, execution_id_, task_.version, TaskStatus::failed, std::nullopt, std::nullopt, std::nullopt,
             progress_.received > 0 ? std::optional<std::string>{progress_.encode()} : std::nullopt, std::nullopt,
             error_json(cause)});
    });
    if (auto ended = adopt(std::move(done))) {
        return *ended;
    }
    return RunReport{RunOutcome::failed, std::string{domain::to_string(cause.code)}};
}

// Ends this logical attempt after a real upstream failure: another one later if attempts
// and the absolute deadline allow, otherwise failed.
RunReport Execution::retry_later(const Error& cause, bool discard_partial) {
    const auto now = s_.clock->now();
    auto backoff = o_.initial_backoff;
    for (std::uint32_t i = 1; i < task_.attempt; ++i) {
        backoff *= std::max<std::uint32_t>(1, o_.backoff_multiplier);
    }
    // Up to a fifth more, so failed tasks do not come back in step.
    thread_local std::minstd_rand jitter{std::random_device{}()};
    backoff += milliseconds{std::uniform_int_distribution<std::int64_t>{0, backoff.count() / 5}(jitter)};
    // A bounded Retry-After of the upstream can only move the next attempt later.
    if (const auto hint = retry_hint(cause)) {
        backoff = std::max<milliseconds>(backoff, std::min(*hint, o_.max_retry_after));
    }
    const auto at = now + backoff;
    if (task_.attempt >= o_.max_attempts || (task_.deadline_at && at >= *task_.deadline_at)) {
        return fail(cause);
    }
    file_.reset();
    ports::DeferCommand command;
    command.task_id = task_.task_id;
    command.execution_id = execution_id_;
    command.expected_version = task_.version;
    command.reason = domain::WaitReason::upstream_retry;
    command.next_retry_at = at;
    command.resume_stage = TaskStatus::resolving;
    command.clear_checkpoint = discard_partial;
    command.error_json = error_json(cause);
    if (auto ended = adopt(write([&] { return s_.tasks->defer(command); }))) {
        return *ended;
    }
    return RunReport{RunOutcome::waiting_retry, std::string{domain::to_string(cause.code)}};
}

// Not admitted now, or the service stops: the I/O has ended, the attempt stays open and
// its running time is kept without the pause.
RunReport Execution::pause(TaskStatus resume, std::chrono::system_clock::time_point when) {
    file_.reset();
    ports::DeferCommand command;
    command.task_id = task_.task_id;
    command.execution_id = execution_id_;
    command.expected_version = task_.version;
    command.reason = domain::WaitReason::admission;
    command.next_retry_at = when;
    command.resume_stage = resume;
    command.attempt_used = task_.attempt_open ? attempt_used() : milliseconds{0};
    if (checkpoint_) {
        command.checkpoint_json = checkpoint_->encode();
    }
    if (progress_.received > 0) {
        auto paused = progress_;
        paused.speed.reset();
        command.progress_json = paused.encode();
    }
    if (auto ended = adopt(write([&] { return s_.tasks->defer(command); }))) {
        return *ended;
    }
    return RunReport{RunOutcome::waiting_admission, "admission"};
}

RunReport Execution::stopped(TaskStatus resume) {
    if (control_.reason.load() == StopReason::canceled) {
        return after_conflict(); // the cancel is committed already: canceling
    }
    // service_shutdown keeps everything resumable and due at once after the restart.
    return pause(resume, s_.clock->now());
}

// A write was refused: a cancel was committed meanwhile (finish it), or the task belongs
// to another execution now (leave it alone).
RunReport Execution::after_conflict() {
    file_.reset();
    auto current = s_.tasks->get(task_.owner_id, task_.task_id);
    if (current && current->status == TaskStatus::canceling && current->execution_id == execution_id_) {
        task_ = std::move(current).value();
        return finish_cancel();
    }
    return RunReport{RunOutcome::left, "conflict"};
}

RunReport Execution::finish_cancel() {
    file_.reset();
    // Unpublished work goes with the cancel; failures leave the directory for diagnosis.
    (void)s_.files->remove_workspace(task_.destination_id, task_.task_id);
    auto done = write([&] {
        return s_.tasks->transition({task_.task_id, execution_id_, task_.version, TaskStatus::canceled, std::nullopt,
                                     std::nullopt, std::nullopt});
    });
    if (!done) {
        return RunReport{RunOutcome::left, std::string{domain::to_string(done.error().code)}};
    }
    task_ = std::move(done).value();
    return RunReport{RunOutcome::canceled, "canceled"};
}

RunReport Execution::expired() {
    if (task_deadline_first_) {
        return fail(error(ErrorCode::deadline_exceeded, "the download did not finish within its time budget", false));
    }
    return retry_later(error(ErrorCode::upstream_timeout, "the attempt did not finish within its time budget", true));
}

RunReport Execution::on_error(const Error& e, TaskStatus resume) {
    if (stop_requested() || e.code == ErrorCode::canceled) {
        return stopped(resume);
    }
    if (admission_deferred(e)) {
        return pause(resume, s_.clock->now() + o_.admission_retry);
    }
    if (e.code == ErrorCode::deadline_exceeded || s_.clock->steady_now() >= deadline_) {
        return expired();
    }
    if (e.retryable) {
        return retry_later(e);
    }
    return fail(e);
}

// Intent, budgets, binding and the attempt (runtime-and-tasks.md, 执行阶段 1).
std::optional<RunReport> Execution::prepare() {
    auto intent = s_.tasks->get_intent(task_.task_id);
    if (!intent) {
        return fail(intent.error());
    }
    intent_ = std::move(intent).value();
    const auto now = s_.clock->now();
    if (task_.deadline_at && now >= *task_.deadline_at) {
        return fail(error(ErrorCode::deadline_exceeded, "the download did not finish within its time budget", false));
    }
    if (!task_.attempt_open && task_.attempt >= o_.max_attempts) {
        return fail(error(ErrorCode::upstream_failed, "no download attempts are left", false));
    }

    const auto& selection = intent_->selection();
    if (std::holds_alternative<domain::AutoFallback>(selection)) {
        return fail(error(ErrorCode::capability_unsupported, "automatic fallback is not enabled", false));
    }
    actual_ = std::holds_alternative<domain::ManualTarget>(selection) ? std::get<domain::ManualTarget>(selection).target
                                                                      : intent_->original_track();
    const auto& bindings = intent_->account_bindings();
    const auto bound = std::find_if(bindings.begin(), bindings.end(),
                                    [&](const domain::AccountBinding& b) { return b.source() == actual_->source; });
    if (bound == bindings.end()) {
        return fail(error(ErrorCode::credentials_required, "the task has no account for its source", false));
    }
    // The fixed binding at its latest valid version, never the owner's current default.
    auto lease = s_.credentials->lease(*bound);
    if (!lease) {
        return fail(lease.error());
    }
    if (lease->secret) {
        secret_ = std::make_shared<const ports::SecretBytes>(std::move(*lease->secret));
        lease->secret.reset();
    }
    lease_ = std::move(lease).value();

    // The attempt starts once the work really does; an open one continues.
    if (auto ended = adopt(write([&] {
            return s_.tasks->begin_attempt({task_.task_id, execution_id_, task_.version, o_.absolute_budget});
        }))) {
        return ended;
    }
    attempt_started_ = s_.clock->steady_now();
    used_before_ = task_.attempt_used;
    const auto attempt_left = duration_cast<milliseconds>(o_.attempt_budget) - used_before_;
    const auto task_left = task_.deadline_at ? duration_cast<milliseconds>(*task_.deadline_at - now) : attempt_left;
    task_deadline_first_ = task_left <= attempt_left;
    deadline_ = attempt_started_ + std::max(milliseconds{0}, std::min(attempt_left, task_left));
    if (attempt_left <= milliseconds{0} || task_left <= milliseconds{0}) {
        return expired();
    }
    checkpoint_ = Checkpoint::decode(task_.checkpoint_json);
    return std::nullopt;
}

// Steps 2-3: a fresh audio address for the actual track (never a stored one).
std::optional<RunReport> Execution::resolve() {
    auto resolver = s_.providers->asset_resolver(actual_->source.value());
    if (!resolver) {
        return fail(resolver.error());
    }
    const ports::ProviderContext call{context(), lease_->identity, std::nullopt, secret_};
    auto audio = resolver.value()->resolve_audio(call, *actual_, intent_->requirement());
    if (!audio) {
        return on_error(audio.error(), resume_stage());
    }
    if (audio->actual_track != *actual_) {
        // Switching tracks while transferring is never done silently.
        return fail(error(ErrorCode::upstream_failed, "the platform returned another track", false));
    }
    auto verdict = domain::evaluate_resource(intent_->requirement(), {audio->actual_quality, audio->completeness});
    if (!verdict) {
        return fail(verdict.error());
    }
    warnings_ = verdict->warnings;
    audio_ = std::move(audio).value();
    return std::nullopt;
}

// The publication name from the track's title and artists; the id when unknown.
std::optional<RunReport> Execution::choose_name() {
    const auto key = track_key(*actual_);
    if (checkpoint_ && checkpoint_->track == key && !checkpoint_->name.empty()) {
        name_ = checkpoint_->name;
        return std::nullopt;
    }
    name_ = s_.file_name(actual_->source.value() + "-" + actual_->id.value());
    auto detail = s_.providers->track_detail(actual_->source.value());
    if (!detail) {
        return std::nullopt;
    }
    const ports::ProviderContext call{context(), lease_->identity, std::nullopt, secret_};
    auto track = detail.value()->track_detail(call, *actual_);
    if (!track) {
        if (stop_requested() || admission_deferred(track.error())) {
            return on_error(track.error(), resume_stage());
        }
        return std::nullopt; // the name is cosmetic; the id stays
    }
    std::string artists;
    for (const auto& artist : track->artists) {
        if (!artist.name.empty()) {
            artists += (artists.empty() ? "" : ", ") + artist.name;
        }
    }
    if (!track->title.empty()) {
        name_ = s_.file_name(artists.empty() ? track->title : artists + " - " + track->title);
    }
    return std::nullopt;
}

std::optional<RunReport> Execution::enter_downloading() {
    const auto quality = audio_->actual_quality ? std::string{domain::to_string(*audio_->actual_quality)} : "unknown";
    // Bytes of another track or quality are never continued.
    if (!checkpoint_ || checkpoint_->track != track_key(*actual_) || checkpoint_->quality != quality) {
        checkpoint_ = Checkpoint{track_key(*actual_), quality, name_, 0, {}, {}, {}, false};
    }
    checkpoint_->name = name_;
    if (task_.status == TaskStatus::downloading) {
        return adopt(write([&] {
            return s_.tasks->checkpoint(
                {task_.task_id, execution_id_, task_.version, std::nullopt, execution_json(), checkpoint_->encode()});
        }));
    }
    ports::TransitionCommand command{task_.task_id, execution_id_, task_.version, TaskStatus::downloading,
                                     std::nullopt,  std::nullopt,  std::nullopt};
    command.progress_json = Progress{}.encode();
    command.execution_json = execution_json();
    return adopt(write([&] { return s_.tasks->transition(command); }));
}

// Flushes what was received so far and commits progress with the durable evidence: the
// bytes reach the device before a checkpoint counts them.
domain::Result<ports::TaskRecord> Execution::write_progress() {
    if (file_) {
        if (auto flushed = file_->flush(); flushed) {
            checkpoint_->durable_bytes = file_->size();
        }
    }
    return s_.tasks->checkpoint(
        {task_.task_id, execution_id_, task_.version, progress_.encode(), std::nullopt, checkpoint_->encode()});
}

std::optional<RunReport> Execution::save_checkpoint(bool flush) {
    if (!flush) {
        return adopt(write([&] {
            return s_.tasks->checkpoint(
                {task_.task_id, execution_id_, task_.version, progress_.encode(), std::nullopt, checkpoint_->encode()});
        }));
    }
    return adopt(write([&] { return write_progress(); }));
}

// Steps 4-5: the work file and the streamed transfer.
std::optional<RunReport> Execution::download() {
    auto& cp = *checkpoint_;
    // Only bytes a committed checkpoint counted are trusted.
    auto opened = s_.work_files->open(task_.destination_id, task_.task_id, std::string{work_file}, cp.durable_bytes);
    if (!opened) {
        return opened.error().code == ErrorCode::destination_unavailable ? retry_later(opened.error())
                                                                         : fail(opened.error());
    }
    file_ = std::move(opened).value();
    if (file_->size() < cp.durable_bytes) {
        cp.durable_bytes = file_->size(); // the file lost bytes it once had: trust what is there
        cp.complete = false;
    }
    if (cp.complete && cp.total_bytes && file_->size() == *cp.total_bytes) {
        progress_ = {file_->size(), cp.total_bytes, {}};
        return std::nullopt; // transfer-complete file of this resource; process it again
    }
    cp.complete = false;

    std::uint32_t retries_left = o_.transport_retries;
    while (true) {
        // Resume only with evidence that the bytes belong to the same resource.
        ResourceIdentity current{cp.track, cp.quality, cp.total_bytes, cp.etag, cp.last_modified, std::nullopt};
        std::optional<PartialFile> partial;
        if (file_->size() > 0) {
            partial = PartialFile{file_->size(), cp.durable_bytes, current};
        }
        const auto decision = decide_resume(partial, current);
        std::uint64_t offset = decision.resume ? decision.offset : 0;
        if (auto cut = file_->truncate(offset); !cut) {
            return fail(cut.error());
        }
        if (!decision.resume) {
            cp.total_bytes.reset();
            cp.etag.reset();
            cp.last_modified.reset();
        }
        cp.durable_bytes = offset;
        if (offset > 0 && cp.total_bytes && offset == *cp.total_bytes) {
            cp.complete = true; // every byte was already here
            progress_ = {offset, cp.total_bytes, {}};
            return save_checkpoint(false);
        }

        ports::HttpRequest request;
        request.url = audio_->url;
        request.operation = "download";
        for (const auto& [name, value] : audio_->request_headers) {
            (application::transport::is_sensitive_header(name) ? request.secret_headers : request.headers)
                .push_back({name, value});
        }
        if (offset > 0) {
            request.headers.push_back({"Range", "bytes=" + std::to_string(offset) + "-"});
            if (decision.if_range) {
                request.headers.push_back({"If-Range", *decision.if_range});
            }
        }
        const ports::AdmissionRequest base{
            task_.owner_id, actual_->source, "download", ports::TrafficClass::download, lease_->identity, {}, {},
            deadline_};

        progress_ = {offset, cp.total_bytes, {}};
        int status = 0;
        std::optional<std::chrono::seconds> wait_hint;   // 429/503 Retry-After
        std::optional<std::uint64_t> unsatisfied_total; // 416 Content-Range: bytes */total
        std::optional<std::string> unsatisfied_etag;
        bool mismatch = false;
        bool too_large = false;
        bool conflict = false; // a cancel or another execution won a checkpoint write
        std::optional<Error> write_error;
        auto last_mark = s_.clock->steady_now();
        auto last_bytes = progress_.received;

        const auto on_head = [&](const ports::HttpResponseHead& head) {
            status = head.status;
            if (status == 200) {
                if (offset > 0) {
                    // The whole body again: never appended to the old prefix.
                    if (auto cut = file_->truncate(0); !cut) {
                        write_error = cut.error();
                        return false;
                    }
                    offset = 0;
                    cp.durable_bytes = 0;
                    last_bytes = 0;
                }
                const auto length = head.header("Content-Length");
                cp.total_bytes = length ? parse_uint(*length) : std::nullopt;
            } else if (status == 206) {
                const auto range = head.header("Content-Range");
                const auto parsed = range ? parse_content_range(*range) : std::nullopt;
                if (offset == 0 || !parsed || parsed->start != offset || !parsed->total ||
                    parsed->total != cp.total_bytes) {
                    mismatch = true; // another range or another resource
                    return false;
                }
            } else {
                if (status == 429 || status == 503) {
                    wait_hint = application::transport::retry_after(head);
                }
                if (status == 416) {
                    const auto range = head.header("Content-Range");
                    if (range && range->rfind("bytes */", 0) == 0) {
                        unsatisfied_total = parse_uint(std::string_view{*range}.substr(8));
                    }
                    unsatisfied_etag = head.header("ETag");
                }
                return false;
            }
            const auto etag = head.header("ETag");
            const auto modified = head.header("Last-Modified");
            if (status == 206 && ((cp.etag && etag && *etag != *cp.etag) ||
                                  (cp.last_modified && modified && *modified != *cp.last_modified))) {
                mismatch = true;
                return false;
            }
            cp.etag = etag;
            cp.last_modified = modified;
            progress_.received = offset;
            progress_.total = cp.total_bytes;
            if (cp.total_bytes && *cp.total_bytes > o_.max_file_bytes) {
                too_large = true;
                return false;
            }
            return true;
        };
        const auto on_chunk = [&](std::span<const char> chunk) {
            if (file_->size() + chunk.size() > o_.max_file_bytes) {
                too_large = true;
                return false;
            }
            if (auto written = file_->write(chunk); !written) {
                write_error = written.error();
                return false;
            }
            progress_.received = file_->size();
            const auto now = s_.clock->steady_now();
            if (now - last_mark >= o_.progress_interval) {
                const auto elapsed = duration_cast<milliseconds>(now - last_mark).count();
                progress_.speed = elapsed > 0
                                      ? (progress_.received - last_bytes) * 1000 / static_cast<std::uint64_t>(elapsed)
                                      : progress_.speed;
                last_mark = now;
                last_bytes = progress_.received;
                // Settled after the stream returns, never inside the transfer callback; a
                // busy database only skips this progress update.
                auto saved = write_progress();
                if (saved) {
                    task_ = std::move(saved).value();
                } else if (saved.error().code == ErrorCode::version_conflict) {
                    conflict = true;
                    return false;
                }
            }
            return !stop_requested();
        };

        auto streamed = s_.upstream->stream(std::move(request), base, context(), on_head, on_chunk);
        if (conflict) {
            return after_conflict(); // canceled or taken over while checkpointing
        }
        if (write_error) {
            return fail(*write_error);
        }
        if (too_large) {
            return fail(error(ErrorCode::validation_failed, "the audio file is larger than allowed", false, "size",
                              "max_file_bytes"));
        }
        if (!streamed) {
            if (stop_requested()) {
                if (auto ended = save_checkpoint(true)) {
                    return ended;
                }
                return stopped(TaskStatus::downloading);
            }
            if (streamed.error().code == ErrorCode::deadline_exceeded || s_.clock->steady_now() >= deadline_) {
                return expired();
            }
            if (!streamed.error().retryable) {
                return fail(streamed.error());
            }
            // An interrupted transfer: retried within the attempt, from what was flushed.
            if (retries_left-- == 0) {
                if (auto ended = save_checkpoint(true)) {
                    return ended;
                }
                return retry_later(streamed.error());
            }
            if (auto ended = save_checkpoint(true)) {
                return ended;
            }
            if (!wait(o_.initial_backoff * (o_.transport_retries - retries_left))) {
                return stopped(TaskStatus::downloading);
            }
            continue;
        }
        if (std::holds_alternative<ports::PendingHop>(streamed.value())) {
            // The next hop (e.g. a CDN redirect) waits for quota: nothing is held meanwhile.
            if (auto ended = save_checkpoint(true)) {
                return ended;
            }
            return pause(TaskStatus::downloading, s_.clock->now() + o_.admission_retry);
        }
        if (stop_requested()) {
            if (auto ended = save_checkpoint(true)) {
                return ended;
            }
            return stopped(TaskStatus::downloading);
        }
        if (status == 416) {
            // Nothing past the offset. Complete only when the server's total is exactly
            // what is already here and the validators agree; verification has the last
            // word. Otherwise the resource is not the one the prefix came from.
            const bool same = unsatisfied_etag ? cp.etag && *unsatisfied_etag == *cp.etag : true;
            if (offset > 0 && unsatisfied_total && cp.total_bytes && *unsatisfied_total == offset &&
                *cp.total_bytes == offset && same && (cp.etag || cp.last_modified)) {
                cp.complete = true;
                progress_ = {offset, cp.total_bytes, {}};
                return save_checkpoint(false);
            }
            mismatch = true;
        }
        if (mismatch) {
            // The resource changed under the partial file: start this resource from zero.
            cp.etag.reset();
            cp.last_modified.reset();
            cp.total_bytes.reset();
            cp.durable_bytes = 0;
            if (auto cut = file_->truncate(0); !cut) {
                return fail(cut.error());
            }
            if (retries_left-- == 0) {
                return retry_later(error(ErrorCode::upstream_failed, "the audio resource changed", true), true);
            }
            continue;
        }
        if (status == 429 || status == 408 || status >= 500) {
            auto e =
                error(status == 429 ? ErrorCode::upstream_rate_limited : ErrorCode::upstream_failed,
                      "the audio server answered with a temporary error", true, "http_status", std::to_string(status));
            if (wait_hint) {
                e.details.push_back({"retry_after_seconds", std::to_string(wait_hint->count())});
            }
            if (auto ended = save_checkpoint(true)) {
                return ended;
            }
            return retry_later(e);
        }
        if (status == 403 || status == 404 || status == 410) {
            // The address expired or was refused: the next attempt resolves a new one.
            if (auto ended = save_checkpoint(true)) {
                return ended;
            }
            return retry_later(error(ErrorCode::resource_expired, "the audio address is no longer valid", true,
                                     "http_status", std::to_string(status)));
        }
        if (status != 200 && status != 206) {
            return fail(error(ErrorCode::upstream_failed, "the audio server refused the download", false, "http_status",
                              std::to_string(status)));
        }
        if (cp.total_bytes && file_->size() != *cp.total_bytes) {
            // A short body: continue from what arrived, within the attempt.
            if (retries_left-- == 0) {
                if (auto ended = save_checkpoint(true)) {
                    return ended;
                }
                return retry_later(error(ErrorCode::upstream_failed, "the audio transfer ended early", true));
            }
            if (auto ended = save_checkpoint(true)) {
                return ended;
            }
            continue;
        }
        if (auto flushed = file_->flush(); !flushed) {
            return fail(flushed.error());
        }
        cp.durable_bytes = file_->size();
        cp.total_bytes = file_->size(); // an unknown length is known once the body ended
        cp.complete = true;
        progress_.received = file_->size();
        progress_.total = progress_.total ? progress_.total : std::optional<std::uint64_t>{file_->size()};
        progress_.speed.reset();
        return save_checkpoint(false);
    }
}

// Step 6: required validation (and later decryption and tags) in the processing pool.
std::optional<RunReport> Execution::process() {
    file_.reset();
    ports::TransitionCommand command{task_.task_id, execution_id_, task_.version, TaskStatus::processing,
                                     std::nullopt,  std::nullopt,  std::nullopt};
    command.progress_json = progress_.encode();
    if (auto ended = adopt(write([&] { return s_.tasks->transition(command); }))) {
        return ended;
    }
    auto processor = s_.processors(audio_->processing);
    if (!processor) {
        return fail(processor.error());
    }
    auto input = s_.work_files->local_path(task_.destination_id,
                                           s_.work_files->relative_path(task_.task_id, std::string{work_file}));
    if (!input) {
        return input.error().code == ErrorCode::destination_unavailable ? retry_later(input.error())
                                                                        : fail(input.error());
    }
    ports::ProcessingRequest request{
        input.value(),
        {audio_->container, audio_->bitrate_bps, audio_->duration_ms,
         checkpoint_->total_bytes ? std::optional<std::int64_t>{static_cast<std::int64_t>(*checkpoint_->total_bytes)}
                                  : std::nullopt,
         audio_->completeness},
        intent_->metadata_policy()};
    const auto ctx = context();

    domain::Result<ports::ProcessingReport> report{
        error(ErrorCode::service_busy, "processing did not run", true, "admission", "processing")};
    if (s_.processing) {
        auto promise = std::make_shared<std::promise<domain::Result<ports::ProcessingReport>>>();
        auto future = promise->get_future();
        auto submitted = s_.processing->submit([promise, worker = processor.value(), request, ctx](std::stop_token) {
            promise->set_value(worker->process(request, ctx));
        });
        if (!submitted) {
            // The pool is full: pause; the transfer-complete file is processed after the claim.
            return pause(TaskStatus::downloading, s_.clock->now() + o_.admission_retry);
        }
        try {
            report = future.get();
        } catch (const std::future_error&) {
            return stopped(TaskStatus::downloading); // the pool dropped it while stopping
        }
    } else {
        report = processor.value()->process(request, ctx);
    }
    if (!report) {
        const auto& e = report.error();
        if (e.code == ErrorCode::audio_invalid) {
            // A cut-off or damaged transfer is fetched again from zero; anything else fails.
            return e.retryable ? retry_later(e, true) : fail(e);
        }
        return on_error(e, TaskStatus::downloading);
    }
    auto verdict = domain::evaluate_resource(intent_->requirement(), {audio_->actual_quality, report->completeness});
    if (!verdict) {
        return fail(verdict.error()); // e.g. measured to be a preview
    }
    for (const auto w : verdict->warnings) {
        if (std::find(warnings_.begin(), warnings_.end(), w) == warnings_.end()) {
            warnings_.push_back(w);
        }
    }
    report_ = std::move(report).value();
    return std::nullopt;
}

// Steps 7-8: the Finalizer reserves, renames and commits completed (or recovery does).
RunReport Execution::publish() {
    if (stop_requested()) {
        return stopped(TaskStatus::downloading); // the last cancel check before publishing
    }
    // The public execution facts first; finalizing then carries them.
    if (auto ended = adopt(write([&] {
            return s_.tasks->checkpoint(
                {task_.task_id, execution_id_, task_.version, std::nullopt, execution_json(), std::nullopt});
        }))) {
        return *ended;
    }
    json properties{{"actual_track", ref_json(*actual_)},
                    {"actual_quality", audio_->actual_quality
                                           ? json(std::string{domain::to_string(*audio_->actual_quality)})
                                           : json(nullptr)},
                    {"container", report_->facts.container},
                    {"bitrate_bps", nullable(report_->facts.bitrate_bps)},
                    {"duration_ms", nullable(report_->facts.duration_ms)}};
    FinalizeRequest request{task_, s_.work_files->relative_path(task_.task_id, std::string{work_file}),
                            name_ + "." + report_->facts.container, *report_, properties.dump()};
    auto published = s_.finalizer->publish(request);
    if (published) {
        task_ = published->task;
        return RunReport{RunOutcome::completed, published->asset.id};
    }
    const auto& e = published.error();
    switch (e.code) {
    case ErrorCode::finalization_failed:
        return RunReport{RunOutcome::failed, std::string{domain::to_string(e.code)}}; // already failed
    case ErrorCode::version_conflict:
        return after_conflict(); // a cancel committed first, or the publication went on
    case ErrorCode::destination_unavailable:
        return retry_later(e); // offline before anything was reserved
    default:
        if (is_move_unconfirmed(e)) {
            return RunReport{RunOutcome::left, std::string{move_unconfirmed}}; // recovery decides
        }
        // Refused before anything was reserved (still processing), or a storage failure after.
        return fail(e);
    }
}

RunReport Execution::run() {
    if (execution_id_.empty() || !domain::is_running(task_.status)) {
        return RunReport{RunOutcome::left, "not claimed"};
    }
    if (auto ended = prepare()) {
        return *ended;
    }
    if (stop_requested()) {
        return stopped(resume_stage());
    }
    if (auto ended = resolve()) {
        return *ended;
    }
    if (auto ended = choose_name()) {
        return *ended;
    }
    if (stop_requested()) {
        return stopped(resume_stage());
    }
    if (auto ended = enter_downloading()) {
        return *ended;
    }
    if (auto ended = download()) {
        return *ended;
    }
    if (stop_requested()) {
        return stopped(TaskStatus::downloading);
    }
    if (auto ended = process()) {
        return *ended;
    }
    return publish();
}

} // namespace

std::string_view to_string(RunOutcome outcome) noexcept {
    switch (outcome) {
    case RunOutcome::completed:
        return "completed";
    case RunOutcome::failed:
        return "failed";
    case RunOutcome::canceled:
        return "canceled";
    case RunOutcome::waiting_admission:
        return "waiting_admission";
    case RunOutcome::waiting_retry:
        return "waiting_retry";
    case RunOutcome::left:
        break;
    }
    return "left";
}

DownloadRunner::DownloadRunner(RunnerOptions options, RunnerServices services)
    : options_(options), services_(std::move(services)) {}

RunReport DownloadRunner::run(const ports::TaskRecord& claimed, ExecutionControl& control) {
    return Execution{options_, services_, claimed, control}.run();
}

} // namespace music_service::downloads
