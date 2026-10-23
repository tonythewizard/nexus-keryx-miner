#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace keryx::nexus {

struct TemplateFields {
  std::string network;
  // Exact values used in the GetBlockTemplate request that produced this
  // snapshot. They are carried with the immutable job so a winning block can
  // bind its payout/escrow configuration to that same template.
  std::string pay_address;
  std::string extra_data;
  std::vector<std::uint8_t> response_wire;
  std::vector<std::uint8_t> block_wire;
  // Canonical consensus-bearing RpcBlock serialization. The network client's
  // producer removes RpcBlock.verboseData (node diagnostics) but retains the
  // complete header, transaction list, proof fields, and unknown protobuf fields.
  std::vector<std::uint8_t> consensus_identity_wire;
  std::uint64_t daa_score{};
  std::uint32_t compact_target_bits{};
  std::uint64_t timestamp{};
  std::uint64_t template_nonce{};
  std::vector<std::string> parent_hashes;
  std::string header_hash_field;
  bool node_synced{};
  std::string inference_challenge;
  std::uint64_t received_monotonic_ns{};
};

class JobLease {
public:
  JobLease() = default;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
  friend class JobPipeline;
  friend class LiveJobQueue;
  JobLease(std::uint64_t generation, std::shared_ptr<std::atomic_bool> active);
  std::uint64_t generation_{};
  std::shared_ptr<std::atomic_bool> active_;
};

struct TemplateJob {
  std::uint64_t generation{};
  TemplateFields fields;
  JobLease lease;
};

// Mining is admissible only for a synchronized mainnet template that does not
// carry an outstanding OPoI challenge. This is a local safety gate, not proof
// that the miner implements the OPoI runtime.
void require_mining_eligible(const TemplateFields &fields);

class JobPipeline {
public:
  TemplateJob accept(TemplateFields fields);
  void invalidate() noexcept;
  void disconnect() noexcept { invalidate(); }
  [[nodiscard]] std::optional<std::uint64_t> current_generation() const noexcept;
  [[nodiscard]] std::uint64_t templates_received() const noexcept { return templates_received_; }
  [[nodiscard]] std::uint64_t invalidations() const noexcept { return invalidations_; }

private:
  std::uint64_t next_generation_{};
  std::uint64_t templates_received_{};
  std::uint64_t invalidations_{};
  std::shared_ptr<std::atomic_bool> current_;
  std::optional<std::uint64_t> current_generation_;
};

} // namespace keryx::nexus
