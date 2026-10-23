#pragma once
#include <functional>

#include "keryx/nexus/network/submit_block_client.hpp"
#include "rpc.pb.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace keryx::nexus::network {

struct PomWinner {
  VerifiedSubmission verification;
  std::uint64_t daa_score{};
  std::uint64_t final_state{};
  std::uint8_t tier{};
  std::string borsh_proof;
};

// Mirrors the official miner's assemble_pom_block: clone the complete node
// template (including coinbase/payout/escrow outputs and serviceStateHash),
// then set only the nonce, proof, and the H3/H6 committed fields.
protowire::RpcBlock assemble_pom_winner(
    const protowire::RpcBlock &template_block, const PomWinner &winner,
    const std::string &payout_address, const std::string &extra_data);

// Shared by the live worker and the loopback mock test. The client still
// enforces every VerifiedSubmission gate and an explicit submit enable bit.
SubmitBlockResult submit_pom_winner(
    SubmitBlockClient &client, const protowire::RpcBlock &template_block,
    const PomWinner &winner, const std::string &payout_address,
    const std::string &extra_data, bool enable_submit,
    std::chrono::seconds timeout = std::chrono::seconds(5),
                           const std::function<bool()> &cancelled = {});

} // namespace keryx::nexus::network
