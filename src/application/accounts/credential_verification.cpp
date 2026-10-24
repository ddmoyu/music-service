#include "application/accounts/credential_service.hpp"
#include "application/ports/runtime/scope_exit.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>

namespace music_service::application::accounts {
namespace {
using domain::Error;
using domain::ErrorCode;

ports::VerificationEvidence unknown(const ports::CredentialRecord& record,
    const ports::Clock& clock, const std::string& reason) {
    ports::VerificationEvidence evidence;
    evidence.at = clock.now();
    evidence.verification_id = record.verification_id;
    evidence.error_code = reason;
    evidence.membership.checked_at = evidence.at;
    evidence.membership.error_code = reason;
    return evidence;
}
} // namespace

struct CredentialService::VerificationState {
    struct Job {
        ports::CredentialRecord record;
        std::stop_source stop;
        std::chrono::steady_clock::time_point deadline;
        bool persistence_failed{false}; // mutex 保护；持久化恢复前查询投影为未知。
    };
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Job>> jobs;
    bool stopping{false};
    std::chrono::milliseconds budget;
};

CredentialService::CredentialService(std::shared_ptr<ports::CredentialRepository> credentials,
    std::shared_ptr<ports::SecretStore> secrets, std::shared_ptr<const ports::ProviderDirectory> providers,
    std::shared_ptr<const ports::Clock> clock, std::shared_ptr<ports::WorkExecutor> executor,
    std::chrono::milliseconds budget)
    : credentials_(std::move(credentials)), secrets_(std::move(secrets)),
      providers_(std::move(providers)), clock_(std::move(clock)) {
    if (executor) {
        if (budget <= std::chrono::milliseconds::zero() || budget > std::chrono::seconds(30))
            throw std::invalid_argument{"invalid credential verification budget"};
        verification_ = std::make_shared<VerificationState>();
        verification_executor_ = std::move(executor);
        verification_->budget = budget;
    }
}

CredentialService::~CredentialService() { stop_verifications(); }

domain::Result<ports::CredentialRecord> CredentialService::schedule_verification(
    const std::string& owner, const std::string& id) {
    if (!verification_) return Error{ErrorCode::feature_unavailable, "credential verification worker is unavailable", false, {}};
    auto state = verification_;
    std::lock_guard lock{state->mutex};
    auto current = credentials_->get(owner, id);
    if (!current) return current.error();
    if (current->verification_id && current->verification_state == "pending") {
        if (auto it = state->jobs.find(*current->verification_id); it != state->jobs.end()) {
            if (!it->second->persistence_failed) return *current;
            auto failed = credentials_->record_verification(owner, id, current->version, unknown(*current, *clock_, "STORAGE_UNAVAILABLE"));
            if (!failed) return failed.error();
            state->jobs.erase(it);
            current = std::move(failed);
        }
    }
    auto pending = credentials_->begin_verification(owner, id, current->version);
    if (!pending) return pending.error();
    for (const auto& [key, job] : state->jobs)
        if (job->record.owner_id == owner && job->record.id == id) job->stop.request_stop();
    std::erase_if(state->jobs, [&](const auto& item) {
        return item.second->persistence_failed && item.second->record.owner_id == owner && item.second->record.id == id;
    });
    auto finish_unknown = [&](const std::string& reason) {
        return credentials_->record_verification(owner, id, pending->version, unknown(*pending, *clock_, reason));
    };
    // 除执行器的 2 个线程、32 项队列外，再限制每个调用方最多 4 项在途请求。
    const auto owned = std::count_if(state->jobs.begin(), state->jobs.end(), [&](const auto& item) {
        return item.second->record.owner_id == owner;
    });
    if (state->stopping || state->jobs.size() >= 34 || owned >= 4) return finish_unknown("SERVICE_BUSY");
    auto job = std::make_shared<VerificationState::Job>();
    job->record = *pending;
    job->deadline = std::chrono::steady_clock::now() + state->budget;
    const auto key = *pending->verification_id;
    state->jobs.emplace(key, job);
    // 只捕获共享依赖，不捕获服务 this；停机时服务与队列的析构顺序不会造成悬空引用。
    auto submitted = verification_executor_->submit([state, job, key, repository = credentials_, secrets = secrets_,
        providers = providers_, clock = clock_](std::stop_token pool_stop) {
        ports::ScopeExit cleanup{[&] {
            std::lock_guard guard{state->mutex};
            if (!job->persistence_failed) state->jobs.erase(key);
        }};
        std::stop_callback stopped{pool_stop, [&] { job->stop.request_stop(); }};
        CredentialService worker{repository, secrets, providers, clock};
        try {
            auto result = worker.verify(job->record.owner_id, job->record.id,
                {job->deadline, job->stop.get_token()}, job->record.version, key);
            if (result || result.error().code == ErrorCode::version_conflict ||
                result.error().code == ErrorCode::validation_failed || result.error().code == ErrorCode::not_found) return;
        } catch (...) { /* 下方尝试保存明确的失败，不输出异常正文。 */ }
        auto failed = repository->record_verification(job->record.owner_id, job->record.id,
            job->record.version, unknown(job->record, *clock, "STORAGE_UNAVAILABLE"));
        if (!failed && failed.error().code != ErrorCode::version_conflict && failed.error().code != ErrorCode::validation_failed) {
            { std::lock_guard guard{state->mutex}; job->persistence_failed = true; }
            throw std::runtime_error{"credential verification result could not be persisted"};
        }
    });
    if (!submitted) {
        state->jobs.erase(key);
        return finish_unknown(std::string{domain::to_string(submitted.error().code)});
    }
    return *pending;
}

domain::Result<ports::CredentialRecord> CredentialService::cancel_verification(
    const std::string& owner, const std::string& id, const std::string& verification_id) {
    if (!verification_) return Error{ErrorCode::feature_unavailable, "credential verification worker is unavailable", false, {}};
    std::lock_guard lock{verification_->mutex};
    auto current = credentials_->get(owner, id);
    if (!current) return current.error();
    if (current->verification_id != verification_id)
        return Error{ErrorCode::version_conflict, "verification was replaced", false, {}};
    if (current->verification_state != "pending") return *current;
    if (auto it = verification_->jobs.find(verification_id); it != verification_->jobs.end()) it->second->stop.request_stop();
    return credentials_->record_verification(owner, id, current->version, unknown(*current, *clock_, "CANCELED"));
}

void CredentialService::stop_verifications() {
    if (!verification_) return;
    std::lock_guard lock{verification_->mutex};
    if (verification_->stopping) return;
    verification_->stopping = true;
    for (const auto& [key, job] : verification_->jobs) {
        job->stop.request_stop();
        // 尚未执行的队列可能被丢弃，也必须结束持久化的 pending；崩溃由启动恢复处理。
        (void)credentials_->record_verification(job->record.owner_id, job->record.id,
            job->record.version, unknown(job->record, *clock_, "INTERRUPTED"));
    }
}

void CredentialService::interrupt_verifications(const std::string& owner, const std::string& id) {
    if (!verification_) return;
    std::lock_guard lock{verification_->mutex};
    for (const auto& [key, job] : verification_->jobs)
        if (job->record.owner_id == owner && job->record.id == id) job->stop.request_stop();
    std::erase_if(verification_->jobs, [&](const auto& item) {
        return item.second->persistence_failed && item.second->record.owner_id == owner && item.second->record.id == id;
    });
}

void CredentialService::interrupt_inactive_verifications(const std::string& owner, const std::string& source) {
    if (!verification_) return;
    std::lock_guard lock{verification_->mutex};
    for (const auto& [key, job] : verification_->jobs) {
        if (job->record.owner_id != owner || job->record.source != source) continue;
        auto current = credentials_->get(owner, job->record.id);
        if (!current || !current->enabled || current->version != job->record.version) job->stop.request_stop();
    }
    std::erase_if(verification_->jobs, [&](const auto& item) {
        return item.second->persistence_failed && item.second->stop.stop_requested();
    });
}

void CredentialService::apply_verification_overlay(ports::CredentialRecord& record) {
    if (!verification_ || !record.verification_id || record.verification_state != "pending") return;
    std::lock_guard lock{verification_->mutex};
    const auto it = verification_->jobs.find(*record.verification_id);
    if (it == verification_->jobs.end() || !it->second->persistence_failed || it->second->record.version != record.version) return;
    record.verification_state = "complete";
    record.last_verify_result = ports::VerifyOutcome::unknown;
    record.verification_error = "STORAGE_UNAVAILABLE";
    record.membership = {};
    record.membership.error_code = "STORAGE_UNAVAILABLE";
}
} // namespace music_service::application::accounts
