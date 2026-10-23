#include "keryx/nexus/pipeline.hpp"
#include <cassert>
#include <stdexcept>

int main() {
  keryx::nexus::JobPipeline pipeline;
  keryx::nexus::TemplateFields f; f.response_wire={1}; f.block_wire={2};
  auto a=pipeline.accept(f); assert(a.generation==1 && a.lease.valid());
  auto b=pipeline.accept(f); assert(b.generation==2 && b.lease.valid() && !a.lease.valid());
  assert(pipeline.templates_received()==2);
  pipeline.disconnect(); assert(!b.lease.valid() && !pipeline.current_generation());
  bool rejected=false; try { pipeline.accept({}); } catch(const std::invalid_argument &) { rejected=true; }
  assert(rejected);

  f.network = "mainnet"; f.node_synced = true;
  keryx::nexus::require_mining_eligible(f);
  f.node_synced = false; rejected=false;
  try { keryx::nexus::require_mining_eligible(f); } catch(const std::invalid_argument &) { rejected=true; }
  assert(rejected);
  f.node_synced = true; f.inference_challenge = "model:nonce"; rejected=false;
  try { keryx::nexus::require_mining_eligible(f); } catch(const std::invalid_argument &) { rejected=true; }
  assert(rejected);
  f.inference_challenge.clear(); f.network = "testnet"; rejected=false;
  try { keryx::nexus::require_mining_eligible(f); } catch(const std::invalid_argument &) { rejected=true; }
  assert(rejected);
}
