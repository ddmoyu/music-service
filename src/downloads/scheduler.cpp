#include "downloads/scheduler.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace music_service::downloads {

namespace {

using Steady = std::chrono::steady_clock;
using std::chrono::milliseconds;

} // namespace

DownloadScheduler::DownloadScheduler(SchedulerOptions options, std::shared_ptr<ports::TaskRepository> tasks,
                                     std::shared_ptr<ports::UpstreamAdmissionPort> admission,
                                     std::shared_ptr<DownloadRunner> runner,
                                     std::shared_ptr<ports::WorkExecutor> executor,
                                     std::shared_ptr<const ports::Clock> clock, RunObserver observer)
    : options_(options), tasks_(std::move(tasks)), admission_(std::move(admission)), runner_(std::move(runner)),
      executor_(std::move(executor)), clock_(std::move(clock)), observer_(std::move(observer)) {
    options_.workers = std::max<std::uint32_t>(1, options_.workers);
}

DownloadScheduler::~DownloadScheduler() {
    stop_claiming();
    join();
}

void DownloadScheduler::start() {
    dispatcher_ = std::jthread{[this](std::stop_token stop) { dispatch(std::move(stop)); }};
}

void DownloadScheduler::wake() {
    {
        const std::lock_guard lock{mutex_};
        woken_ = true;
    }
    changed_.notify_all();
}

void DownloadScheduler::request_stop(const std::string& task_id) {
    const std::lock_guard lock{mutex_};
    if (const auto it = running_.find(task_id); it != running_.end()) {
        it->second->request(StopReason::canceled);
    }
}

void DownloadScheduler::set_paused(bool paused) {
    {
        const std::lock_guard lock{mutex_};
        paused_ = paused;
        woken_ = true;
    }
    changed_.notify_all();
}

bool DownloadScheduler::paused() const {
    const std::lock_guard lock{mutex_};
    return paused_;
}

void DownloadScheduler::stop_claiming() {
    const std::lock_guard lock{mutex_};
    claiming_ = false;
}

bool DownloadScheduler::wait_idle(milliseconds budget) {
    std::unique_lock lock{mutex_};
    return idle_.wait_for(lock, budget, [this] { return running_.empty(); });
}

void DownloadScheduler::stop_running() {
    const std::lock_guard lock{mutex_};
    for (const auto& [task_id, control] : running_) {
        control->request(StopReason::shutdown);
    }
}

void DownloadScheduler::join() {
    if (dispatcher_.joinable()) {
        dispatcher_.request_stop();
        changed_.notify_all();
        dispatcher_.join();
    }
}

DownloadScheduler::Stats DownloadScheduler::stats() const {
    const std::lock_guard lock{mutex_};
    return Stats{options_.workers, static_cast<std::uint32_t>(running_.size()), paused_, claimed_};
}

void DownloadScheduler::dispatch(std::stop_token stop) {
    while (!stop.stop_requested()) {
        const auto delay = std::clamp(fill() - clock_->steady_now(), Steady::duration{milliseconds{10}},
                                      Steady::duration{options_.scan_interval});
        std::unique_lock lock{mutex_};
        changed_.wait_for(lock, stop, delay, [this] { return woken_; });
        woken_ = false;
    }
}

Steady::time_point DownloadScheduler::fill() {
    const auto rescan = clock_->steady_now() + options_.scan_interval;
    // Bounded: tasks claimed by nobody else cannot keep this loop going for ever.
    for (std::uint32_t round = 0; round < options_.workers * 4; ++round) {
        {
            const std::lock_guard lock{mutex_};
            if (!claiming_ || paused_ || running_.size() >= options_.workers) {
                return rescan;
            }
        }
        auto candidates = tasks_->runnable(clock_->now(), options_.candidates_per_owner, options_.candidates);
        if (!candidates) {
            return rescan; // the database is busy: the next scan tries again
        }
        auto retry_at = rescan;
        const auto pick = choose(candidates.value(), retry_at);
        if (!pick) {
            return retry_at;
        }
        auto claimed = tasks_->claim(pick->task_id, clock_->now());
        if (!claimed || !claimed.value()) {
            continue; // canceled or claimed meanwhile
        }
        auto task = std::move(*claimed.value());
        auto control = std::make_shared<ExecutionControl>();
        {
            const std::lock_guard lock{mutex_};
            running_[task.task_id] = control;
            ++claimed_;
        }
        auto submitted = executor_->submit([this, task, control](std::stop_token pool_stop) {
            // The pool stops only at shutdown: the execution pauses, it is not canceled.
            const std::stop_callback on_pool_stop{pool_stop, [&control] { control->request(StopReason::shutdown); }};
            const auto report = runner_->run(task, *control);
            if (observer_) {
                observer_(task, report);
            }
            finished(task.task_id);
        });
        if (!submitted) {
            finished(task.task_id);
            // Nobody runs it: give the claim back without counting anything.
            ports::DeferCommand back;
            back.task_id = task.task_id;
            back.execution_id = task.execution_id.value_or("");
            back.expected_version = task.version;
            back.next_retry_at = clock_->now() + options_.scan_interval;
            back.resume_stage =
                task.status == domain::TaskStatus::processing ? domain::TaskStatus::downloading : task.status;
            back.attempt_used = task.attempt_used;
            (void)tasks_->defer(back);
            return rescan;
        }
    }
    return clock_->steady_now();
}

std::optional<ports::RunnableTask> DownloadScheduler::choose(const std::vector<ports::RunnableTask>& candidates,
                                                             Steady::time_point& retry_at) {
    std::map<std::string, std::vector<const ports::RunnableTask*>> by_owner;
    for (const auto& task : candidates) {
        by_owner[task.owner_id].push_back(&task);
    }
    // Principals take turns: start after the one served last, wrap around.
    std::vector<const std::string*> order;
    for (auto it = by_owner.upper_bound(last_owner_); it != by_owner.end(); ++it) {
        order.push_back(&it->first);
    }
    for (auto it = by_owner.begin(); it != by_owner.end() && it->first <= last_owner_; ++it) {
        order.push_back(&it->first);
    }

    std::map<std::pair<std::string, std::string>, bool> ready; // (owner, source) -> may run now
    for (const auto* owner : order) {
        for (const auto* task : by_owner[*owner]) {
            const auto key = std::pair{task->owner_id, task->source};
            auto known = ready.find(key);
            if (known == ready.end()) {
                bool runnable = true;
                if (auto source = domain::SourceId::parse(task->source); source && admission_) {
                    const ports::AdmissionRequest probe{task->owner_id,
                                                        std::move(source).value(),
                                                        "download",
                                                        ports::TrafficClass::download,
                                                        std::nullopt,
                                                        {},
                                                        {},
                                                        clock_->steady_now() + std::chrono::minutes{1}};
                    if (const auto blocked = admission_->readiness(probe)) {
                        runnable = false;
                        retry_at = std::min(retry_at, blocked->retry_at);
                    }
                }
                known = ready.emplace(key, runnable).first;
            }
            if (known->second) {
                last_owner_ = *owner;
                return *task;
            }
        }
    }
    return std::nullopt;
}

void DownloadScheduler::finished(const std::string& task_id) {
    {
        const std::lock_guard lock{mutex_};
        running_.erase(task_id);
        woken_ = true; // a slot is free
    }
    changed_.notify_all();
    idle_.notify_all();
}

} // namespace music_service::downloads
