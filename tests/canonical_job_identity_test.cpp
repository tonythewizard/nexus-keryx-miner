#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/live_job_queue.hpp"
#include "messages.pb.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
keryx::nexus::consensus::Hash reference_job_fingerprint(
    const keryx::nexus::TemplateFields &fields) {
  std::vector<std::uint8_t> framed;
  const auto append_u64 = [&](std::uint64_t x) {
    for (unsigned i = 0; i < 8; i++)
      framed.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
  };
  append_u64(fields.response_wire.size());
  framed.insert(framed.end(), fields.response_wire.begin(),
                fields.response_wire.end());
  append_u64(fields.block_wire.size());
  framed.insert(framed.end(), fields.block_wire.begin(),
                fields.block_wire.end());
  return keryx::nexus::consensus::blake3_hash(framed);
}
} // namespace

int main() {
  try {
    protowire::RpcBlock base;
    auto *h = base.mutable_header();
    h->set_version(1);
    h->add_parents()->add_parenthashes(std::string(64, '1'));
    h->set_hashmerkleroot(std::string(64, '2'));
    h->set_acceptedidmerkleroot(std::string(64, '3'));
    h->set_utxocommitment(std::string(64, '4'));
    h->set_timestamp(1000); h->set_bits(0x1d00ffff); h->set_nonce(7);
    h->set_daascore(2000); h->set_bluework("abcd"); h->set_pruningpoint(std::string(64, '5'));
    h->set_bluescore(3000); h->set_pomfinalstate(9);
    h->set_servicestatehash(std::string(64, '6')); h->set_pomtier(2);
    auto *tx = base.add_transactions();
    tx->set_version(1); tx->set_locktime(10); tx->set_subnetworkid("subnet");
    tx->set_payload("payload"); tx->set_mass(55);
    const auto canonical = keryx::nexus::canonical_consensus_block_identity(base);

    auto verbose_only = base;
    verbose_only.mutable_verbosedata()->set_hash("diagnostic hash");
    verbose_only.mutable_verbosedata()->set_difficulty(123.0);
    verbose_only.mutable_verbosedata()->set_ischainblock(true);
    if (canonical != keryx::nexus::canonical_consensus_block_identity(verbose_only))
      throw std::runtime_error("verbose node diagnostics changed canonical consensus identity");

    using Mutation = std::pair<const char *, std::function<void(protowire::RpcBlock &)>>;
    const std::vector<Mutation> mutations{
      {"version", [](auto &b){ b.mutable_header()->set_version(2); }},
      {"parents", [](auto &b){ b.mutable_header()->mutable_parents(0)->set_parenthashes(0, std::string(64, 'a')); }},
      {"merkle root", [](auto &b){ b.mutable_header()->set_hashmerkleroot(std::string(64, 'a')); }},
      {"accepted ID root", [](auto &b){ b.mutable_header()->set_acceptedidmerkleroot(std::string(64, 'a')); }},
      {"UTXO commitment", [](auto &b){ b.mutable_header()->set_utxocommitment(std::string(64, 'a')); }},
      {"timestamp/seed", [](auto &b){ b.mutable_header()->set_timestamp(1001); }},
      {"target bits", [](auto &b){ b.mutable_header()->set_bits(0x1d00fffe); }},
      {"template nonce", [](auto &b){ b.mutable_header()->set_nonce(8); }},
      {"DAA/seed era", [](auto &b){ b.mutable_header()->set_daascore(2001); }},
      {"blue work", [](auto &b){ b.mutable_header()->set_bluework("abce"); }},
      {"pruning point", [](auto &b){ b.mutable_header()->set_pruningpoint(std::string(64, 'a')); }},
      {"blue score", [](auto &b){ b.mutable_header()->set_bluescore(3001); }},
      {"PoM final state", [](auto &b){ b.mutable_header()->set_pomfinalstate(10); }},
      {"service commitment", [](auto &b){ b.mutable_header()->set_servicestatehash(std::string(64, 'a')); }},
      {"tier/model binding", [](auto &b){ b.mutable_header()->set_pomtier(3); }},
      {"transaction", [](auto &b){ b.mutable_transactions(0)->set_payload("different"); }}
    };
    for (const auto &[name, mutate] : mutations) {
      auto changed = base;
      mutate(changed);
      if (canonical == keryx::nexus::canonical_consensus_block_identity(changed))
        throw std::runtime_error(std::string("consensus field omitted from canonical identity: ") + name);
    }

    keryx::nexus::LiveJobQueue queue;
    keryx::nexus::TemplateFields fields;
    fields.network = "mainnet";
    fields.node_synced = true;
    fields.pay_address = "keryx:test";
    fields.extra_data = "extra";
    fields.response_wire = {0x00, 0x01, 0x7f, 0x80, 0xff};
    fields.block_wire = canonical;
    fields.consensus_identity_wire = canonical;
    queue.publish(fields);
    auto job = *queue.wait_pop();
    const keryx::nexus::consensus::Hash pph{0xaa, 0xbb, 0xcc};
    const auto bound = keryx::nexus::consensus::bind_candidate(job, 42, 123456, pph);
    if (bound.job_fingerprint != reference_job_fingerprint(job.fields))
      throw std::runtime_error("streaming bind_candidate fingerprint differs from reference framing");
    if (keryx::nexus::consensus::candidate_binding_status(bound, job) !=
        keryx::nexus::consensus::CandidateBindingStatus::Valid)
      throw std::runtime_error("valid candidate binding rejected");
    auto mutated_job = job;
    mutated_job.fields.block_wire.back() ^= 1U;
    if (keryx::nexus::consensus::candidate_binding_status(bound, mutated_job) !=
        keryx::nexus::consensus::CandidateBindingStatus::IntegrityMismatch)
      throw std::runtime_error("mutated block_wire accepted by candidate_binding_status");

    std::cout << "CANONICAL_JOB_IDENTITY=PASS consensus_fields=" << mutations.size()
              << " verbose_diagnostics_excluded=true parent_order_preserved=true"
              << " candidate_binding_stream_parity=true\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "CANONICAL_JOB_IDENTITY=FAIL " << e.what() << '\n';
    return 1;
  }
}
