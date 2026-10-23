#include "keryx/nexus/live_job_queue.hpp"

#include <iostream>
#include <stdexcept>

using namespace keryx::nexus;

TemplateFields fields(std::uint8_t marker, std::uint8_t response_marker = 0) {
  TemplateFields f;
  f.network = "mainnet";
  f.response_wire = {response_marker ? response_marker : marker, 1};
  f.block_wire = {marker, 2};
  f.consensus_identity_wire = {marker, 2};
  f.node_synced = true;
  return f;
}

int main() {
  try {
    LiveJobQueue queue;
    if (queue.publish(fields(1)) != LiveJobQueue::PublishResult::Published)
      throw std::runtime_error("first job was not published");
    auto first = queue.wait_pop();
    if (!first || first->generation != 1 || !first->lease.valid())
      throw std::runtime_error("first job snapshot/lease invalid");
    if (!queue.begin_refresh() || queue.admission_allowed(1) || !first->lease.valid())
      throw std::runtime_error("refresh must pause admission without prematurely invalidating the lease");
    if (queue.begin_refresh())
      throw std::runtime_error("duplicate notification was not coalesced while refresh was pending");
    if (queue.publish(fields(1, 77)) != LiveJobQueue::PublishResult::Duplicate)
      throw std::runtime_error("duplicate template was not deduplicated");
    if (!first->lease.valid()) throw std::runtime_error("duplicate invalidated active job");
    if (queue.current_generation() != 1 || !queue.admission_allowed(1))
      throw std::runtime_error("identical canonical job changed generation or failed to resume");
    auto resumed = queue.wait_pop();
    if (!resumed || resumed->generation != 1 || !resumed->lease.valid())
      throw std::runtime_error("identical refresh did not resume the same job lease");

    auto changed = fields(2);
    changed.consensus_identity_wire = {2, 9}; // a consensus-bearing header/parent/target change
    if (queue.publish(std::move(changed)) != LiveJobQueue::PublishResult::Published)
      throw std::runtime_error("replacement job was not published");
    if (first->lease.valid()) throw std::runtime_error("new generation did not stale old work");
    auto second = queue.wait_pop();
    if (!second || second->generation != 2 || !second->lease.valid())
      throw std::runtime_error("second generation invalid");

    queue.invalidate_for_virtual_chain_change();
    if (second->lease.valid() || queue.current_generation())
      throw std::runtime_error("selected-parent change left an active generation");
    if (queue.admission_allowed(3))
      throw std::runtime_error("virtual-chain refresh admitted work before the replacement snapshot");
    if (queue.publish(fields(3)) != LiveJobQueue::PublishResult::Published)
      throw std::runtime_error("reconnected job was not published");
    auto third = queue.wait_pop();
    if (!third || third->generation != 3 || !third->lease.valid())
      throw std::runtime_error("generation did not remain monotone across reconnect");
    queue.close();
    if (third->lease.valid() || queue.wait_pop())
      throw std::runtime_error("cooperative close did not invalidate and wake consumer");
    if (queue.published_count() != 3 || queue.duplicate_count() != 1)
      throw std::runtime_error("queue counters mismatch");
    std::cout << "LIVE_JOB_QUEUE=PASS canonical_duplicate_same_generation=true duplicate_refresh_coalesced=true virtual_chain_immediate_invalidation=true monotone_generations=3 stale_cancel=PASS close=PASS\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "LIVE_JOB_QUEUE=FAIL " << e.what() << '\n';
    return 1;
  }
}
