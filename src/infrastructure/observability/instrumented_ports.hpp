#pragma once

#include "application/ports/observability/metrics.hpp"
#include "application/ports/processing/audio_processor.hpp"
#include "application/ports/transport/http_transport.hpp"

namespace music_service::infrastructure::observability {
namespace ports = application::ports;

class ObservedTransport final : public ports::HttpTransport {
public:
    ObservedTransport(std::shared_ptr<ports::HttpTransport> inner, std::shared_ptr<ports::Metrics> metrics)
        : inner_(std::move(inner)), metrics_(std::move(metrics)) {}
    domain::Result<ports::HttpResponse> request(const ports::HttpRequest& request,
        const ports::UpstreamLease& lease, const ports::OperationContext& context) override {
        ports::MetricTimer timer{metrics_, ports::MetricOperation::upstream};
        auto result = inner_->request(request, lease, context);
        timer.outcome = result ? ports::http_metric_outcome(result->head.status) : ports::metric_outcome(result.error());
        return result;
    }
    domain::Result<ports::HttpResponseHead> receive(const ports::HttpRequest& request,
        const ports::UpstreamLease& lease, const ports::OperationContext& context,
        const ports::HeadConsumer& on_head, const ports::ChunkConsumer& on_chunk) override {
        ports::MetricTimer timer{metrics_, ports::MetricOperation::upstream};
        auto result = inner_->receive(request, lease, context, on_head, on_chunk);
        timer.outcome = result ? ports::http_metric_outcome(result->status) : ports::metric_outcome(result.error());
        return result;
    }
private:
    std::shared_ptr<ports::HttpTransport> inner_;
    std::shared_ptr<ports::Metrics> metrics_;
};
class ObservedProcessor final : public ports::AudioProcessor {
public:
    ObservedProcessor(std::shared_ptr<ports::AudioProcessor> inner, std::shared_ptr<ports::Metrics> metrics)
        : inner_(std::move(inner)), metrics_(std::move(metrics)) {}
    ports::ProcessingPlan plan() const override { return inner_->plan(); }
    domain::Result<ports::ProcessingReport> process(const ports::ProcessingRequest& request,
        const ports::OperationContext& context) override {
        ports::MetricTimer timer{metrics_, ports::MetricOperation::processing};
        auto result = inner_->process(request, context);
        timer.outcome = result ? ports::MetricOutcome::success : ports::metric_outcome(result.error());
        return result;
    }
private:
    std::shared_ptr<ports::AudioProcessor> inner_;
    std::shared_ptr<ports::Metrics> metrics_;
};
} // namespace music_service::infrastructure::observability
