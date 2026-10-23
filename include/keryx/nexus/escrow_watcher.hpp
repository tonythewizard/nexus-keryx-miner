#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "rpc.pb.h"

namespace keryx::nexus::ai {

std::uint64_t query_escrow_virtual_daa(std::string_view endpoint);
protowire::GetUtxosByAddressesResponseMessage query_escrow_utxos(std::string_view endpoint, const std::vector<protowire::RpcOutpoint>&);

struct EscrowBlockResult {
  std::optional<protowire::RpcTransaction> claim;
  std::uint64_t pending_count{};
  std::uint64_t pending_amount_sompi{};
};

// Thin owner for the exact, vendored stock v0.5.7 EscrowWatcher. It never sends a
// transaction itself; callers must gate claim submission and feed the RPC result back.
class EscrowWatcher final {
public:
  EscrowWatcher(const std::array<std::uint8_t, 32> &secret,
                std::string_view payout,
                const std::filesystem::path &state_path);
  ~EscrowWatcher();
  EscrowWatcher(const EscrowWatcher &) = delete;
  EscrowWatcher &operator=(const EscrowWatcher &) = delete;
  EscrowWatcher(EscrowWatcher &&) = delete;
  EscrowWatcher &operator=(EscrowWatcher &&) = delete;

  std::vector<std::array<std::uint8_t, 32>> start_state_validation();
  bool validation_result(const std::array<std::uint8_t, 32> &hash, bool is_selected_chain);
  bool validation_error(std::string_view error);
  void chain_block_removed(const std::array<std::uint8_t, 32> &hash);
  EscrowBlockResult handle_block(const protowire::RpcBlock &block);
  // `error == nullopt` is the node's positive mempool acknowledgement, not chain finality.
  int submit_result(std::string_view txid, std::optional<std::string_view> error);
  void set_claim_construction_enabled(bool enabled);
  bool validation_in_progress() const;
  void track_inference_outpoint(std::string_view txid, std::uint64_t daa,
                                std::uint64_t amount_sompi);
  void flush();
  std::vector<protowire::RpcOutpoint> claim_outpoints(std::uint64_t daa);
  std::optional<protowire::RpcTransaction> verified_claim(const protowire::RpcBlock&, const protowire::GetUtxosByAddressesResponseMessage&);


private:
  void *handle_{};
  mutable std::mutex mutex_;
};

} // namespace keryx::nexus::ai
