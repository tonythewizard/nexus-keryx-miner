#include "keryx/nexus/escrow_watcher.hpp"

#include "keryx/nexus/ai_service.hpp"
#include "keryx/nexus/escrow_bridge.h"

#include <algorithm>
#include "messages.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <unordered_set>
#include <stdexcept>

namespace keryx::nexus::ai {
namespace {
std::string hex(const std::uint8_t *data, std::size_t size) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out(size * 2, '\0');
  for (std::size_t i = 0; i < size; ++i) {
    out[2 * i] = digits[data[i] >> 4U];
    out[2 * i + 1] = digits[data[i] & 0x0fU];
  }
  return out;
}

void require_ok(int rc, const char *operation) {
  if (rc != 0) throw std::runtime_error(std::string("stock EscrowWatcher ") + operation +
      " failed (bridge status " + std::to_string(rc) + ")");
}
}

// Native node v1.6.4 read-only RPC; Rust consumes its compatible UTXO payload.
std::uint64_t query_escrow_virtual_daa(std::string_view endpoint) {
  if (!endpoint.starts_with("grpc://127.0.0.1:"))
    throw std::runtime_error("escrow maturity requires local node endpoint");
  auto stub=protowire::RPC::NewStub(grpc::CreateChannel(std::string(endpoint.substr(7)),grpc::InsecureChannelCredentials()));
  const auto deadline=std::chrono::system_clock::now()+std::chrono::seconds(5);
  auto rpc=[&](protowire::KaspadRequest request) {
    grpc::ClientContext ctx;ctx.set_deadline(deadline);
    auto stream=stub->MessageStream(&ctx);protowire::KaspadResponse response;
    if(!stream||!stream->Write(request)||!stream->WritesDone()||!stream->Read(&response)||response.id()!=request.id()) {
      ctx.TryCancel();throw std::runtime_error("escrow maturity RPC unavailable/correlation failed");
    }
    if(!stream->Finish().ok())throw std::runtime_error("escrow maturity RPC failed");
    return response;
  };
  protowire::KaspadRequest info;info.set_id(1);info.mutable_getinforequest();auto ir=rpc(info);
  if(!ir.has_getinforesponse()||ir.getinforesponse().has_error()||!ir.getinforesponse().issynced())
    throw std::runtime_error("escrow maturity requires synced node");
  protowire::KaspadRequest dag;dag.set_id(2);dag.mutable_getblockdaginforequest();auto dr=rpc(dag);
  if(!dr.has_getblockdaginforesponse()||dr.getblockdaginforesponse().has_error()||dr.getblockdaginforesponse().virtualdaascore()==0)
    throw std::runtime_error("escrow virtual DAA unavailable");
  return dr.getblockdaginforesponse().virtualdaascore();
}

protowire::GetUtxosByAddressesResponseMessage query_escrow_utxos(std::string_view endpoint, const std::vector<protowire::RpcOutpoint>& ops) {
  if(endpoint != "grpc://127.0.0.1:22110" && !endpoint.starts_with("grpc://127.0.0.1:"))
    throw std::runtime_error("escrow UTXO query requires local verified node endpoint");
  if(ops.size()>10000)throw std::runtime_error("bounded escrow UTXO list exhausted");
  protowire::GetUtxosByAddressesResponseMessage combined;
  auto channel=grpc::CreateChannel(std::string(endpoint.substr(7)),grpc::InsecureChannelCredentials());
  auto stub=protowire::RPC::NewStub(channel);
  const auto deadline=std::chrono::system_clock::now()+std::chrono::seconds(15);
  std::unordered_set<std::string> seen;
  for(std::size_t begin=0;begin<ops.size();begin+=128) {
    const auto end=std::min(ops.size(),begin+128);
    std::unordered_set<std::string> requested;
    grpc::ClientContext ctx;ctx.set_deadline(deadline);auto stream=stub->MessageStream(&ctx);
    protowire::KaspadRequest q;q.set_id(begin+1);
    for(auto i=begin;i<end;++i) {
      *q.mutable_getutxoentriesbyoutpointsrequest()->add_outpoints()=ops[i];
      requested.insert(ops[i].transactionid()+":"+std::to_string(ops[i].index()));
    }
    if(!stream||!stream->Write(q)||!stream->WritesDone()){ctx.TryCancel();throw std::runtime_error("escrow UTXO request unavailable");}
    protowire::KaspadResponse r;
    if(!stream->Read(&r)||r.id()!=q.id()){ctx.TryCancel();(void)stream->Finish();throw std::runtime_error("escrow UTXO response correlation failed");}
    auto status=stream->Finish();if(!status.ok())throw std::runtime_error("escrow UTXO RPC: "+status.error_message());
    protowire::GetUtxosByAddressesResponseMessage part;
    if(!r.has_getutxoentriesbyoutpointsresponse())throw std::runtime_error("escrow UTXO RPC unsupported by node");
    if(!part.ParseFromString(r.getutxoentriesbyoutpointsresponse().SerializeAsString()))throw std::runtime_error("escrow UTXO malformed response");
    if(part.has_error())throw std::runtime_error("escrow UTXO node error: "+part.error().message());
    for(const auto &e:part.entries()) {
      if(!e.has_outpoint()||!e.has_utxoentry()||!e.utxoentry().has_scriptpublickey())throw std::runtime_error("escrow UTXO entry incomplete");
      auto key=e.outpoint().transactionid()+":"+std::to_string(e.outpoint().index());
      if(!requested.contains(key)||!seen.insert(key).second)throw std::runtime_error("escrow UTXO unsolicited/duplicate entry");
      *combined.add_entries()=e;
    }
  }
  return combined;
}
std::vector<protowire::RpcOutpoint> EscrowWatcher::claim_outpoints(std::uint64_t daa) {
  std::lock_guard lock(mutex_);std::vector<std::uint8_t> bytes(1024*1024);std::size_t size=0;
  require_ok(nexus_escrow_claim_outpoints(handle_,daa,bytes.data(),bytes.size(),&size),"pending outpoints");
  protowire::RpcTransaction tx;if(!tx.ParseFromArray(bytes.data(),static_cast<int>(size)))throw std::runtime_error("pending outpoints decode failed");
  std::vector<protowire::RpcOutpoint> result;for(const auto &i:tx.inputs())result.push_back(i.previousoutpoint());return result;
}
std::optional<protowire::RpcTransaction> EscrowWatcher::verified_claim(const protowire::RpcBlock &b,const protowire::GetUtxosByAddressesResponseMessage &u) {
  std::lock_guard lock(mutex_);auto block=b.SerializeAsString(),live=u.SerializeAsString();std::vector<std::uint8_t> bytes(65536);std::size_t size=0;
  require_ok(nexus_escrow_verified_claim(handle_,reinterpret_cast<const std::uint8_t*>(block.data()),block.size(),reinterpret_cast<const std::uint8_t*>(live.data()),live.size(),bytes.data(),bytes.size(),&size),"UTXO verified claim");
  if(!size)return std::nullopt;protowire::RpcTransaction tx;
  if(!tx.ParseFromArray(bytes.data(),static_cast<int>(size)))throw std::runtime_error("verified claim decode failed");return tx;
}

EscrowWatcher::EscrowWatcher(const std::array<std::uint8_t, 32> &secret,
                             std::string_view payout,
                             const std::filesystem::path &state_path) {
  if (!state_path.is_absolute() || state_path.string().starts_with("/tmp/"))
    throw std::invalid_argument("escrow state path must be absolute and persistent outside /tmp");
  const auto secret_text = hex(secret.data(), secret.size());
  const auto path_text = state_path.string();
  handle_ = nexus_escrow_watcher_new(
      reinterpret_cast<const std::uint8_t *>(secret_text.data()), secret_text.size(),
      reinterpret_cast<const std::uint8_t *>(payout.data()), payout.size(),
      reinterpret_cast<const std::uint8_t *>(path_text.data()), path_text.size());
  if (!handle_) throw std::runtime_error("stock EscrowWatcher rejected escrow identity or state");
}

EscrowWatcher::~EscrowWatcher() {
  if (handle_) {
    (void)nexus_escrow_flush(handle_);
    nexus_escrow_watcher_free(handle_);
  }
}

std::vector<std::array<std::uint8_t, 32>> EscrowWatcher::start_state_validation() {
  std::lock_guard lock(mutex_);
  std::size_t count = 0;
  const int query = nexus_escrow_start_validation(handle_, nullptr, 0, &count);
  if (query != 0 && query != 1) require_ok(query, "start validation sizing");
  std::vector<std::uint8_t> bytes(count * 32);
  require_ok(nexus_escrow_start_validation(handle_, bytes.empty() ? nullptr : bytes.data(), bytes.size(), &count),
             "start validation");
  if (bytes.size() != count * 32) throw std::runtime_error("stock escrow validation hash count changed");
  std::vector<std::array<std::uint8_t, 32>> hashes(count);
  for (std::size_t i = 0; i < count; ++i)
    std::copy_n(bytes.data() + i * 32, 32, hashes[i].begin());
  return hashes;
}

bool EscrowWatcher::validation_result(const std::array<std::uint8_t, 32> &hash, bool selected) {
  std::lock_guard lock(mutex_);
  const auto result = nexus_escrow_validation_result(handle_, hash.data(), selected ? 1 : 0);
  if (result < 0) require_ok(result, "selected-chain validation");
  return result == 1;
}

bool EscrowWatcher::validation_error(std::string_view error) {
  std::lock_guard lock(mutex_);
  const auto result = nexus_escrow_validation_error(handle_,
      reinterpret_cast<const std::uint8_t *>(error.data()), error.size());
  if (result < 0) require_ok(result, "unknown-block validation");
  return result == 1;
}

void EscrowWatcher::chain_block_removed(const std::array<std::uint8_t, 32> &hash) {
  std::lock_guard lock(mutex_);
  require_ok(nexus_escrow_chain_block_removed(handle_, hash.data()), "selected-chain reorg");
}

EscrowBlockResult EscrowWatcher::handle_block(const protowire::RpcBlock &block) {
  std::lock_guard lock(mutex_);
  std::string wire;
  if (!block.SerializeToString(&wire)) throw std::runtime_error("cannot serialize block for stock escrow watcher");
  std::size_t claim_size = 0;
  EscrowBlockResult result;
  auto rc = nexus_escrow_handle_block(handle_, reinterpret_cast<const std::uint8_t *>(wire.data()), wire.size(),
      nullptr, 0, &claim_size, &result.pending_count, &result.pending_amount_sompi);
  if (rc != 0 && rc != 1) require_ok(rc, "block scan sizing");
  if (claim_size == 0) {
    require_ok(rc, "block scan");
    return result;
  }
  std::vector<std::uint8_t> claim(claim_size);
  rc = nexus_escrow_handle_block(handle_, reinterpret_cast<const std::uint8_t *>(wire.data()), wire.size(),
      claim.empty() ? nullptr : claim.data(), claim.size(), &claim_size,
      &result.pending_count, &result.pending_amount_sompi);
  require_ok(rc, "block scan");
  if (claim_size != claim.size()) throw std::runtime_error("stock escrow claim size changed during read");
  if (!claim.empty()) {
    protowire::RpcTransaction tx;
    if (!tx.ParseFromArray(claim.data(), static_cast<int>(claim.size())))
      throw std::runtime_error("stock escrow claim transaction failed protobuf decode");
    result.claim = std::move(tx);
  }
  return result;
}

int EscrowWatcher::submit_result(std::string_view txid, std::optional<std::string_view> error) {
  std::lock_guard lock(mutex_);
  const auto *error_data = error ? reinterpret_cast<const std::uint8_t *>(error->data()) : nullptr;
  const auto result = nexus_escrow_submit_result(handle_,
      reinterpret_cast<const std::uint8_t *>(txid.data()), txid.size(), error_data,
      error ? error->size() : 0);
  if (result < 0) require_ok(result, "submit response reconciliation");
  return result;
}

void EscrowWatcher::set_claim_construction_enabled(bool enabled) {
  std::lock_guard lock(mutex_);
  require_ok(nexus_escrow_set_claims_enabled(handle_, enabled ? 1 : 0), "claim construction policy");
}

bool EscrowWatcher::validation_in_progress() const {
  std::lock_guard lock(mutex_);
  const auto result = nexus_escrow_validation_in_progress(handle_);
  if (result < 0) require_ok(result, "validation state query");
  return result != 0;
}

void EscrowWatcher::track_inference_outpoint(std::string_view txid, std::uint64_t daa,
                                             std::uint64_t amount_sompi) {
  std::lock_guard lock(mutex_);
  require_ok(nexus_escrow_track_inference_outpoint(handle_,
      reinterpret_cast<const std::uint8_t *>(txid.data()), txid.size(), daa, amount_sompi),
      "inference outpoint tracking");
}

void EscrowWatcher::flush() { std::lock_guard lock(mutex_); require_ok(nexus_escrow_flush(handle_), "durable flush"); }

} // namespace keryx::nexus::ai
