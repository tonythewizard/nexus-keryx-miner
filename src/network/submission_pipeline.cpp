#include "keryx/nexus/network/submission_pipeline.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>

namespace keryx::nexus::network {
namespace {
constexpr std::uint64_t h3_activation_daa = 43'450'000;
constexpr std::uint64_t h6_activation_daa = 76'316'623;

bool is_hex(std::string_view value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isxdigit(c) != 0;
  });
}

void validate_h6_mining_fields(std::uint64_t daa, const std::string &payout,
                               const std::string &extra,
                               const protowire::RpcBlockHeader &header) {
  if (payout.empty()) throw std::invalid_argument("payout address is not configured");
  if (daa < h6_activation_daa) return;
  if (header.servicestatehash().size() != 64 || !is_hex(header.servicestatehash()))
    throw std::invalid_argument("H6 template lacks the node serviceStateHash commitment");
  constexpr std::string_view escrow_tag = "/escrow:";
  constexpr std::string_view signature_tag = "/esig:";
  const auto escrow = extra.find(escrow_tag);
  if (escrow == std::string::npos || escrow + escrow_tag.size() + 64 > extra.size())
    throw std::invalid_argument("H6 mining extraData lacks the official escrow public key");
  const auto pubkey = std::string_view(extra).substr(escrow + escrow_tag.size(), 64);
  if (!is_hex(pubkey)) throw std::invalid_argument("H6 escrow public key is not hex");
  const auto signature = extra.find(signature_tag, escrow + escrow_tag.size() + 64);
  if (signature == std::string::npos || signature + signature_tag.size() + 128 > extra.size())
    throw std::invalid_argument("H6 mining extraData lacks the delegation signature");
  const auto cert = std::string_view(extra).substr(signature + signature_tag.size(), 128);
  if (!is_hex(cert)) throw std::invalid_argument("H6 escrow delegation signature is not hex");
}
}

protowire::RpcBlock assemble_pom_winner(
    const protowire::RpcBlock &template_block, const PomWinner &winner,
    const std::string &payout_address, const std::string &extra_data) {
  if (!template_block.has_header())
    throw std::invalid_argument("winning template has no header");
  if (winner.borsh_proof.empty())
    throw std::invalid_argument("winning candidate has no serialized PoM proof");
  if (winner.verification.nonce == 0)
    throw std::invalid_argument("winning candidate nonce is missing");
  const auto &source_header = template_block.header();
  if (source_header.daascore() != winner.daa_score)
    throw std::invalid_argument("winner DAA does not match the template");
  validate_h6_mining_fields(winner.daa_score, payout_address, extra_data,
                            source_header);

  protowire::RpcBlock result;
  result.CopyFrom(template_block);
  auto *header = result.mutable_header();
  header->set_nonce(winner.verification.nonce);
  if (winner.daa_score >= h3_activation_daa)
    header->set_pomfinalstate(winner.final_state);
  if (winner.daa_score >= h6_activation_daa)
    header->set_pomtier(winner.tier);
  result.set_pomproof(winner.borsh_proof);

  // The template is authoritative for service-state, coinbase, payout and
  // escrow outputs. Verify that the only protobuf fields changed are the PoM
  // winner fields above; never synthesize coinbase or delegation data.
  if (result.header().servicestatehash() != source_header.servicestatehash())
    throw std::logic_error("block assembly changed node serviceStateHash");
  if (result.transactions_size() != template_block.transactions_size())
    throw std::logic_error("block assembly changed template transactions");
  for (int i = 0; i < result.transactions_size(); ++i) {
    if (result.transactions(i).SerializeAsString() !=
        template_block.transactions(i).SerializeAsString())
      throw std::logic_error("block assembly changed payout/escrow transaction data");
  }
  return result;
}

SubmitBlockResult submit_pom_winner(
    SubmitBlockClient &client, const protowire::RpcBlock &template_block,
    const PomWinner &winner, const std::string &payout_address,
    const std::string &extra_data, bool enable_submit,
    std::chrono::seconds timeout, const std::function<bool()> &cancelled) {
  if (!winner.verification.target_satisfied)
    throw std::invalid_argument("candidate does not satisfy target; SubmitBlock not invoked");
  if (!winner.verification.job_fresh || !winner.verification.generation_matches)
    throw std::invalid_argument("candidate job is stale or generation-mismatched");
  const auto assembly_start=std::chrono::steady_clock::now();
  auto block = assemble_pom_winner(template_block, winner, payout_address,
                                   extra_data);
  const auto assembly_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-assembly_start).count();
  auto result=client.submit(block, winner.verification, enable_submit, timeout, cancelled);
  result.timing.assembly_ms=assembly_ms;
  return result;
}

} // namespace keryx::nexus::network
