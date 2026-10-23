#include "keryx/nexus/pipeline.hpp"

#include <stdexcept>
#include <utility>

namespace keryx::nexus {

void require_mining_eligible(const TemplateFields &fields) {
  if (fields.network != "mainnet")
    throw std::invalid_argument("CUDA mining requires an authenticated mainnet template");
  if (!fields.node_synced)
    throw std::invalid_argument("CUDA mining is blocked because the node is not synchronized");
  if (!fields.inference_challenge.empty())
    throw std::invalid_argument("CUDA mining is blocked by an outstanding OPoI inference challenge");
}

JobLease::JobLease(std::uint64_t generation, std::shared_ptr<std::atomic_bool> active)
    : generation_(generation), active_(std::move(active)) {}

bool JobLease::valid() const noexcept {
  return active_ && active_->load(std::memory_order_acquire);
}

TemplateJob JobPipeline::accept(TemplateFields fields) {
  if (fields.response_wire.empty() || fields.block_wire.empty())
    throw std::invalid_argument("a job requires the complete template response and block bytes");
  invalidate();
  const auto generation = ++next_generation_;
  current_ = std::make_shared<std::atomic_bool>(true);
  current_generation_ = generation;
  ++templates_received_;
  JobLease lease(generation, current_);
  return TemplateJob{generation, std::move(fields), std::move(lease)};
}

void JobPipeline::invalidate() noexcept {
  if (current_) {
    current_->store(false, std::memory_order_release);
    current_.reset();
    current_generation_.reset();
    ++invalidations_;
  }
}

std::optional<std::uint64_t> JobPipeline::current_generation() const noexcept {
  return current_generation_;
}

} // namespace keryx::nexus
