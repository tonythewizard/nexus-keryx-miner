#include "keryx/nexus/platform/io.hpp"
#include "keryx/nexus/ai_response_service.hpp"
#include <chrono>
#include <utility>

#include "messages.grpc.pb.h"

#include <curl/curl.h>
#include <blake2.h>
#include <openssl/sha.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <zlib.h>

extern "C" {
int nexus_ai_open_request(const std::uint8_t *, std::size_t, const std::uint8_t *,
                          std::uint8_t *, std::size_t *, std::uint8_t *, std::uint8_t *);
int nexus_ai_seal_private_response(const std::uint8_t *, const std::uint8_t *, const std::uint8_t *,
                                  const std::uint8_t *, std::size_t, std::uint8_t *, std::size_t *);
int nexus_ai_build_signed_response(const std::uint8_t *, const std::uint8_t *, const std::uint8_t *,
                                   std::uint64_t, const std::uint8_t *, std::size_t,
                                   const std::uint8_t *, std::size_t, std::uint8_t *, std::size_t *);
int nexus_ai_verify_response(const std::uint8_t *, std::size_t, const std::uint8_t *,
                             const std::uint8_t *, const std::uint8_t *, std::uint8_t *, std::size_t *);
int nexus_ai_verify_escrow_delegation(const std::uint8_t *, std::size_t,
                                     const std::uint8_t *, const std::uint8_t *);
std::uint64_t nexus_ai_max_answer_len(std::uint32_t);
int nexus_ai_response_request_hash(const std::uint8_t *, std::size_t, std::uint8_t *);
int nexus_ai_escrow_pubkey_from_secret(const std::uint8_t *, std::uint8_t *);
}

namespace keryx::nexus::ai {
namespace {
std::string sha256_hex(std::string_view value) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char *>(value.data()), value.size(), digest.data());
  static constexpr char hex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (std::size_t i = 0; i < digest.size(); ++i) {
    out[i * 2] = hex[digest[i] >> 4]; out[i * 2 + 1] = hex[digest[i] & 15U];
  }
  return out;
}

std::optional<std::vector<std::uint8_t>> decode_hex(std::string_view in) {
  if ((in.size() & 1U) != 0) return std::nullopt;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::vector<std::uint8_t> out(in.size() / 2);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = nibble(in[i * 2]), lo = nibble(in[i * 2 + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

std::string encode_hex(std::span<const std::uint8_t> in) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out(in.size() * 2, '0');
  for (std::size_t i = 0; i < in.size(); ++i) {
    out[i * 2] = digits[in[i] >> 4]; out[i * 2 + 1] = digits[in[i] & 15U];
  }
  return out;
}

std::string state_field(const ResponseJournalRecord &record) {
  return record.request_id + "|" + record.transaction_id + "|" + response_state_name(record.state);
}

std::optional<ResponseState> parse_state(std::string_view value) {
  for (int i = static_cast<int>(ResponseState::Discovered);
       i <= static_cast<int>(ResponseState::ResponseReorged); ++i) {
    const auto candidate = static_cast<ResponseState>(i);
    if (value == response_state_name(candidate)) return candidate;
  }
  return std::nullopt;
}

bool terminal(ResponseState state) {
  return state == ResponseState::RpcAcknowledged || state == ResponseState::RpcRejected ||
         state == ResponseState::Finalized || state == ResponseState::Expired ||
         state == ResponseState::Failed || state == ResponseState::NotAssigned;
}

bool allowed_transition(ResponseState from, ResponseState to) {
  if (to == ResponseState::Failed || to == ResponseState::Expired) return !terminal(from);
  switch (from) {
    case ResponseState::Discovered: return to == ResponseState::Assigned || to == ResponseState::NotAssigned;
    case ResponseState::Assigned: return to == ResponseState::RequestValidated;
    case ResponseState::RequestValidated: return to == ResponseState::Inferencing;
    case ResponseState::Inferencing: return to == ResponseState::ResponsePrepared;
    case ResponseState::ResponsePrepared: return to == ResponseState::ResponseSigned || to == ResponseState::ResponsePublished;
    case ResponseState::ResponseSigned: return to == ResponseState::ResponsePublished || to == ResponseState::TransactionPrepared;
    case ResponseState::ResponsePublished: return to == ResponseState::ResponseSigned || to == ResponseState::TransactionPrepared;
    case ResponseState::TransactionPrepared: return to == ResponseState::Submitting;
    case ResponseState::Submitting: return to == ResponseState::RpcAcknowledged || to == ResponseState::RpcRejected || to == ResponseState::UnknownAfterTimeout;
    case ResponseState::UnknownAfterTimeout: return to == ResponseState::RpcAcknowledged || to == ResponseState::RpcRejected || to == ResponseState::ChainObserved;
    case ResponseState::RpcAcknowledged: return to == ResponseState::ChainObserved || to == ResponseState::Finalized;
    case ResponseState::ChainObserved: return to == ResponseState::Finalized || to == ResponseState::ResponseReorged;
    case ResponseState::ResponseReorged: return to == ResponseState::ChainObserved;
    case ResponseState::RpcRejected: case ResponseState::Finalized: case ResponseState::Expired:
    case ResponseState::Failed: case ResponseState::NotAssigned: return false;
  }
  return false;
}

void append_le(std::string &out, std::uint64_t n, unsigned width) {
  for (unsigned i = 0; i < width; ++i) out.push_back(static_cast<char>(n >> (i * 8U)));
}

std::string official_txid(const protowire::RpcTransaction &tx) {
  std::string raw;
  append_le(raw, tx.version(), 2);
  append_le(raw, 0, 8); // input count; AiResponse has no inputs
  append_le(raw, 0, 8); // output count; AiResponse has no outputs
  append_le(raw, tx.locktime(), 8);
  const auto subnetwork = decode_hex(tx.subnetworkid());
  const auto payload = decode_hex(tx.payload());
  if (!subnetwork || subnetwork->size() != 20 || !payload)
    throw std::runtime_error("AiResponse transaction encoding invalid before txid calculation");
  raw.append(reinterpret_cast<const char *>(subnetwork->data()), subnetwork->size());
  append_le(raw, tx.gas(), 8);
  append_le(raw, payload->size(), 8);
  raw.append(reinterpret_cast<const char *>(payload->data()), payload->size());
  std::array<std::uint8_t, 32> digest{};
  constexpr std::string_view key = "TransactionID";
  if (blake2b(digest.data(), raw.data(), key.data(), digest.size(), raw.size(), key.size()) != 0)
    throw std::runtime_error("AiResponse TransactionID hashing failed");
  return encode_hex(digest);
}

std::string prompt_from_private(std::span<const std::uint8_t> input) {
  static constexpr std::array<std::uint8_t, 4> magic{0x00, 'K', 'Z', 0x01};
  if (input.size() < magic.size() || !std::equal(magic.begin(), magic.end(), input.begin()))
    return std::string(reinterpret_cast<const char *>(input.data()), input.size());
  // Stock v0.5.7 uses raw DEFLATE and caps the inflated prompt at 1 MiB.
  z_stream stream{};
  stream.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(input.data() + magic.size()));
  stream.avail_in = static_cast<uInt>(input.size() - magic.size());
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) throw std::runtime_error("private prompt inflater initialization failed");
  std::string output;
  std::array<char, 16384> chunk{};
  int rc = Z_OK;
  while (rc == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef *>(chunk.data());
    stream.avail_out = static_cast<uInt>(chunk.size());
    rc = inflate(&stream, Z_NO_FLUSH);
    const auto produced = chunk.size() - stream.avail_out;
    if (produced > (1U << 20) - std::min<std::size_t>(output.size(), 1U << 20)) {
      inflateEnd(&stream); throw std::runtime_error("private prompt exceeds stock 1 MiB expansion bound");
    }
    output.append(chunk.data(), produced);
  }
  inflateEnd(&stream);
  if (rc != Z_STREAM_END) throw std::runtime_error("private prompt raw DEFLATE stream is invalid");
  return output;
}

std::array<std::uint8_t, 34> sha256_multihash(std::span<const std::uint8_t> bytes) {
  std::array<std::uint8_t, 34> cid{};
  cid[0] = 0x12; cid[1] = 0x20;
  SHA256(bytes.data(), bytes.size(), cid.data() + 2);
  return cid;
}

struct CurlBuffer { std::string bytes; std::size_t limit{4*1024*1024}; };
size_t curl_write(char *data, size_t size, size_t count, void *user) {
  auto &buffer=*static_cast<CurlBuffer *>(user);
  if(size && count>std::numeric_limits<std::size_t>::max()/size)return 0;
  const auto n = size * count;
  if(n>buffer.limit-buffer.bytes.size())return 0;
  try {buffer.bytes.append(data,n);} catch (...) {return 0;}
  return n;
}

std::pair<std::string, bool> endpoint_target(std::string_view endpoint) {
  bool tls = false;
  std::string target;
  if (endpoint.starts_with("grpcs://")) { tls = true; target = endpoint.substr(8); }
  else if (endpoint.starts_with("grpc://")) target = endpoint.substr(7);
  else throw std::invalid_argument("AI response RPC endpoint requires grpc:// loopback or grpcs:// TLS");
  if (!tls) {
    const auto colon = target.rfind(':');
    if (colon == std::string::npos) throw std::invalid_argument("AI response endpoint has no port");
    const auto host = target.substr(0, colon);
    in_addr addr{};
    if (inet_pton(AF_INET, host.c_str(), &addr) != 1 || (ntohl(addr.s_addr) >> 24U) != 127U)
      throw std::invalid_argument("plaintext AI response transport is restricted to numeric IPv4 loopback");
  }
  return {target, tls};
}
}

const char *response_state_name(ResponseState state) noexcept {
  switch (state) {
    case ResponseState::Discovered: return "DISCOVERED"; case ResponseState::Assigned: return "ASSIGNED";
    case ResponseState::RequestValidated: return "REQUEST_VALIDATED"; case ResponseState::Inferencing: return "INFERENCING";
    case ResponseState::ResponsePrepared: return "RESPONSE_PREPARED"; case ResponseState::ResponseSigned: return "RESPONSE_SIGNED";
    case ResponseState::ResponsePublished: return "RESPONSE_PUBLISHED"; case ResponseState::TransactionPrepared: return "TRANSACTION_PREPARED";
    case ResponseState::Submitting: return "SUBMITTING"; case ResponseState::RpcAcknowledged: return "RPC_ACKNOWLEDGED";
    case ResponseState::RpcRejected: return "RPC_REJECTED"; case ResponseState::UnknownAfterTimeout: return "UNKNOWN_AFTER_TIMEOUT";
    case ResponseState::ChainObserved: return "CHAIN_OBSERVED"; case ResponseState::Finalized: return "FINALIZED";
    case ResponseState::Expired: return "EXPIRED"; case ResponseState::Failed: return "FAILED"; case ResponseState::NotAssigned: return "NOT_ASSIGNED";
    case ResponseState::ResponseReorged: return "RESPONSE_REORGED";
  }
  return "UNKNOWN";
}

ResponseJournal::ResponseJournal(std::filesystem::path path) : path_(std::filesystem::absolute(std::move(path)).lexically_normal()) {
  const auto text = path_.string();
  if (text == "/tmp" || text.starts_with("/tmp/")) throw std::invalid_argument("response journal must not use /tmp");
}

void ResponseJournal::recover() {
  std::lock_guard lock(mutex_);
  records_.clear();
  std::ifstream in(path_);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    const auto last = line.rfind('|');
    if (last == std::string::npos || sha256_hex(std::string_view(line).substr(0, last)) != line.substr(last + 1))
      throw std::runtime_error("AiResponse journal checksum mismatch; refusing recovery");
    const auto first = line.find('|');
    const auto second = line.find('|', first + 1);
    if (first == std::string::npos || second == std::string::npos) throw std::runtime_error("AiResponse journal record malformed");
    const auto state = parse_state(std::string_view(line).substr(second + 1, last - second - 1));
    if (!state || first != 64 || (second - first - 1 != 0 && second - first - 1 != 64))
      throw std::runtime_error("AiResponse journal record fields invalid");
    ResponseJournalRecord record{line.substr(0, first), line.substr(first + 1, second - first - 1), *state};
    if (record.request_id.find_first_not_of("0123456789abcdef") != std::string::npos ||
        (!record.transaction_id.empty() && record.transaction_id.find_first_not_of("0123456789abcdef") != std::string::npos))
      throw std::runtime_error("AiResponse journal identifiers malformed");
    records_[record.request_id] = std::move(record);
  }
}

bool ResponseJournal::begin(std::string id) {
  std::lock_guard lock(mutex_);
  if (records_.contains(id)) return false;
  ResponseJournalRecord record{std::move(id), {}, ResponseState::Discovered};
  append_locked(record);
  records_.emplace(record.request_id, record);
  return true;
}

bool ResponseJournal::reconsider_unsubmitted_not_assigned(std::string_view id) {
  std::lock_guard lock(mutex_);auto it=records_.find(std::string(id));
  if(it==records_.end()||it->second.state!=ResponseState::NotAssigned||!it->second.transaction_id.empty())return false;
  ResponseJournalRecord reset{std::string(id),{},ResponseState::Discovered};
  append_locked(reset);it->second=std::move(reset);return true;
}

bool ResponseJournal::reset_pre_submission(std::string_view id) {
  std::lock_guard lock(mutex_);
  auto it = records_.find(std::string(id));
  if (it == records_.end() || terminal(it->second.state) ||
      it->second.state == ResponseState::Submitting ||
      it->second.state == ResponseState::UnknownAfterTimeout ||
      it->second.state == ResponseState::RpcAcknowledged ||
      it->second.state == ResponseState::RpcRejected ||
      it->second.state == ResponseState::ChainObserved ||
      it->second.state == ResponseState::Finalized) return false;
  ResponseJournalRecord reset{std::string(id), {}, ResponseState::Discovered};
  append_locked(reset);
  it->second = std::move(reset);
  return true;
}

bool ResponseJournal::transition(std::string_view id, ResponseState next, std::string txid) {
  std::lock_guard lock(mutex_);
  auto it = records_.find(std::string(id));
  if (it == records_.end() || !allowed_transition(it->second.state, next)) return false;
  if (!txid.empty() && (txid.size() != 64 || txid.find_first_not_of("0123456789abcdef") != std::string::npos))
    throw std::invalid_argument("AiResponse journal transaction ID must be lowercase hex");
  auto updated = it->second;
  if (!txid.empty()) updated.transaction_id = std::move(txid);
  updated.state = next;
  append_locked(updated);
  it->second = std::move(updated);
  return true;
}

bool ResponseJournal::mark_response_reorged(std::string_view id) {
  return transition(id, ResponseState::ResponseReorged);
}

std::optional<ResponseJournalRecord> ResponseJournal::get(std::string_view id) const {
  std::lock_guard lock(mutex_); const auto it = records_.find(std::string(id));
  if (it == records_.end()) return std::nullopt;
  return it->second;
}

std::vector<ResponseJournalRecord> ResponseJournal::unresolved() const {
  std::lock_guard lock(mutex_); std::vector<ResponseJournalRecord> out;
  for (const auto &[_, record] : records_) if (!terminal(record.state)) out.push_back(record);
  return out;
}

std::vector<ResponseJournalRecord> ResponseJournal::snapshot() const {
  std::lock_guard lock(mutex_);
  std::vector<ResponseJournalRecord> out;
  out.reserve(records_.size());
  for (const auto &[_, record] : records_) out.push_back(record);
  std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
    return a.request_id < b.request_id;
  });
  return out;
}

void ResponseJournal::append_locked(const ResponseJournalRecord &record) {
  std::filesystem::create_directories(path_.parent_path());
  const auto body = state_field(record);
  const auto line = body + "|" + sha256_hex(body) + "\n";
  const int fd = keryx::nexus::platform::io::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("cannot open AiResponse journal");
  std::size_t offset = 0;
  while (offset < line.size()) {
    const auto n = keryx::nexus::platform::io::write(fd, line.data() + offset, line.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { keryx::nexus::platform::io::close(fd); throw std::runtime_error("AiResponse journal write failed"); }
    offset += static_cast<std::size_t>(n);
  }
  if (keryx::nexus::platform::io::fsync(fd) != 0) { keryx::nexus::platform::io::close(fd); throw std::runtime_error("AiResponse journal fsync failed"); }
  keryx::nexus::platform::io::close(fd);
}

int stock_open_request(std::span<const std::uint8_t> payload, const std::array<std::uint8_t, 32> &secret,
                       std::vector<std::uint8_t> &prompt, std::array<std::uint8_t, 32> &root, bool &is_private) {
  std::size_t length = 0; std::uint8_t private_flag = 0;
  int rc = nexus_ai_open_request(payload.data(), payload.size(), secret.data(), nullptr, &length, root.data(), &private_flag);
  if (rc != 2 && rc != 0) return rc;
  prompt.resize(length); is_private = private_flag != 0;
  rc = nexus_ai_open_request(payload.data(), payload.size(), secret.data(), prompt.data(), &length, root.data(), &private_flag);
  if (rc == 0) prompt.resize(length); else prompt.clear();
  return rc;
}

std::vector<std::uint8_t> stock_seal_private_response(const std::array<std::uint8_t, 32> &root,
    const std::array<std::uint8_t, 32> &request, const std::array<std::uint8_t, 32> &secret,
    std::span<const std::uint8_t> answer) {
  std::size_t length = 0;
  int rc = nexus_ai_seal_private_response(root.data(), request.data(), secret.data(), answer.data(), answer.size(), nullptr, &length);
  if (rc != 2 && rc != 0) throw std::runtime_error("official private response sealing failed");
  std::vector<std::uint8_t> out(length);
  rc = nexus_ai_seal_private_response(root.data(), request.data(), secret.data(), answer.data(), answer.size(), out.data(), &length);
  if (rc != 0) throw std::runtime_error("official private response sealing failed");
  out.resize(length); return out;
}

std::vector<std::uint8_t> stock_build_signed_response(const std::array<std::uint8_t, 32> &request,
    const std::array<std::uint8_t, 32> &secret, const std::array<std::uint8_t, 34> &cid,
    std::uint64_t daa, std::span<const std::uint8_t> answer, std::span<const std::uint8_t> body) {
  std::size_t length = 0;
  int rc = nexus_ai_build_signed_response(request.data(), secret.data(), cid.data(), daa,
      answer.data(), answer.size(), body.data(), body.size(), nullptr, &length);
  if (rc != 2 && rc != 0) throw std::runtime_error("official AiResponse payload builder failed");
  std::vector<std::uint8_t> out(length);
  rc = nexus_ai_build_signed_response(request.data(), secret.data(), cid.data(), daa,
      answer.data(), answer.size(), body.data(), body.size(), out.data(), &length);
  if (rc != 0) throw std::runtime_error("official AiResponse payload serialization failed");
  out.resize(length); return out;
}

bool stock_verify_response(std::span<const std::uint8_t> payload, const std::array<std::uint8_t, 32> &request,
    const std::array<std::uint8_t, 32> &pubkey, const std::optional<std::array<std::uint8_t, 32>> &root,
    std::vector<std::uint8_t> *answer) {
  std::size_t length = answer ? answer->size() : 0;
  const int rc = nexus_ai_verify_response(payload.data(), payload.size(), request.data(), pubkey.data(),
      root ? root->data() : nullptr, answer && !answer->empty() ? answer->data() : nullptr, &length);
  if (rc == 2 && answer) {
    answer->resize(length);
    const int retry = nexus_ai_verify_response(payload.data(), payload.size(), request.data(), pubkey.data(),
        root ? root->data() : nullptr, answer->data(), &length);
    if (retry != 0) return false;
    answer->resize(length); return true;
  }
  return rc == 0;
}

bool stock_verify_response_signature(std::span<const std::uint8_t> payload,
    const std::array<std::uint8_t, 32> &request, const std::array<std::uint8_t, 32> &pubkey) {
  std::size_t unused = 0;
  const int rc = nexus_ai_verify_response(payload.data(), payload.size(), request.data(),
      pubkey.data(), nullptr, nullptr, &unused);
  // The pinned Rust bridge returns -8 only AFTER request/responder binding and
  // Schnorr authentication succeed, when a private answer has no requester root.
  // Full verification/decryption above retains its original stricter contract.
  constexpr int authenticated_private_answer_without_root = -8;
  return rc == 0 || rc == authenticated_private_answer_without_root;
}

AiResponseService::AiResponseService(AiResponseConfig config)
    : config_(std::move(config)), journal_(config_.journal_path) {}
void AiResponseService::recover() { journal_.recover(); }
bool AiResponseService::discover(const protowire::RpcTransaction &transaction,
                                 std::uint64_t containing_daa) {
  const auto request=parse_request(transaction,containing_daa,nullptr,config_.network);
  return request ? journal_.begin(request->stable_id) : false;
}
std::size_t AiResponseService::observe_block(const protowire::RpcBlock &block) {
  constexpr std::string_view response_subnetwork="0400000000000000000000000000000000000000";
  std::size_t observed=0;
  for(const auto &tx:block.transactions()) {
    if(tx.subnetworkid()!=response_subnetwork)continue;
    const auto payload=decode_hex(tx.payload());if(!payload)continue;
    const auto request_hash=stock_response_request_hash(*payload);if(!request_hash)continue;
    const auto trace=stock_response_trace(*payload);
    const auto id=hex_encode(request_hash->data(),request_hash->size());
    const auto record=journal_.get(id);if(!record)continue;
    if(record->state!=ResponseState::RpcAcknowledged&&record->state!=ResponseState::UnknownAfterTimeout&&
       record->state!=ResponseState::ChainObserved&&record->state!=ResponseState::ResponseReorged)continue;
    if(!stock_verify_response_signature(*payload,*request_hash,config_.escrow_pubkey))continue;
    if(trace)std::cout<<"AI_RESPONSE_TRACE_OBSERVED request="<<id<<" stage="<<unsigned(trace->stage)
                     <<" positions="<<trace->total_positions<<" correctness=OPENING_REQUIRED\n"<<std::flush;
    if(block.has_verbosedata()&&!block.verbosedata().hash().empty()) {
      const auto block_hash=block.verbosedata().hash();
      std::lock_guard lock(observed_mutex_);
      response_blocks_[id].insert(block_hash);
      blocks_by_response_[block_hash].insert(id);
    }
    if(record->state!=ResponseState::ChainObserved&&journal_.transition(id,ResponseState::ChainObserved))++observed;
  }
  return observed;
}

std::size_t AiResponseService::observe_removed_chain_blocks(const std::vector<std::string> &hashes) {
  std::unordered_set<std::string> candidates;
  {
    std::lock_guard lock(observed_mutex_);
    for(const auto &hash:hashes) {
      const auto block=blocks_by_response_.find(hash);
      if(block==blocks_by_response_.end())continue;
      for(const auto &id:block->second) {
        candidates.insert(id);
        const auto appearances=response_blocks_.find(id);
        if(appearances!=response_blocks_.end()) {
          appearances->second.erase(hash);
          if(appearances->second.empty())response_blocks_.erase(appearances);
        }
      }
      blocks_by_response_.erase(block);
    }
  }
  std::size_t reorged=0;
  for(const auto &id:candidates) {
    bool remains=false;
    {std::lock_guard lock(observed_mutex_);const auto it=response_blocks_.find(id);remains=it!=response_blocks_.end()&&!it->second.empty();}
    if(!remains&&journal_.mark_response_reorged(id))++reorged;
  }
  return reorged;
}

bool stock_verify_escrow_delegation(std::string_view payout,
    const std::array<std::uint8_t, 32> &escrow, const std::array<std::uint8_t, 64> &signature) {
  return nexus_ai_verify_escrow_delegation(
      reinterpret_cast<const std::uint8_t *>(payout.data()), payout.size(),
      escrow.data(), signature.data()) == 0;
}

std::array<std::uint8_t, 32> stock_escrow_pubkey_from_secret(
    const std::array<std::uint8_t, 32> &secret) {
  std::array<std::uint8_t, 32> result{};
  if (nexus_ai_escrow_pubkey_from_secret(secret.data(), result.data()) != 0)
    throw std::invalid_argument("invalid existing escrow secret key");
  return result;
}

std::optional<std::array<std::uint8_t, 32>> stock_response_request_hash(
    std::span<const std::uint8_t> payload) {
  std::array<std::uint8_t,32> result{};
  if(nexus_ai_response_request_hash(payload.data(),payload.size(),result.data())!=0)return std::nullopt;
  return result;
}

namespace {
template<class F>
auto measured_phase(const AiResponseEffects &effects, std::string_view phase, F &&operation) {
  struct Timer {
    const AiResponseEffects &effects;
    std::string_view phase;
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    ~Timer() noexcept {
      if (!effects.phase_timing) return;
      const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now()-started).count();
      try { effects.phase_timing(phase, static_cast<std::uint64_t>(ns)); } catch (...) {}
    }
  } timer{effects, phase};
  return std::forward<F>(operation)();
}
} // namespace

ServeResult AiResponseService::serve(const protowire::RpcTransaction &tx, std::uint64_t daa,
    const std::vector<ServiceProvider> &providers, const AiResponseEffects &effects,
    std::optional<std::uint64_t> deadline, std::optional<std::uint64_t> current_daa) {
  if (!effects.infer || !effects.publish_and_readback || !effects.submit || !effects.reconcile) return ServeResult::Failed;
  std::string why;
  auto parsed = parse_request(tx, daa, &why, config_.network);
  if (!parsed) return ServeResult::Failed;
  if (!stock_verify_escrow_delegation(config_.payout_address, config_.escrow_pubkey,
                                      config_.delegation_signature)) return ServeResult::Failed;
  if (parsed->model_id != config_.model_id) return ServeResult::NotAssigned;
  const auto id = parsed->stable_id;
  const auto assignment = classify_assignment(*parsed, config_.service_identity,
      hex_encode(config_.escrow_pubkey.data(), config_.escrow_pubkey.size()), config_.tier, providers);
  if (!journal_.begin(id)) {
    const auto old = journal_.get(id);
    if (!old) return ServeResult::Failed;
    bool reset_done = false;
    if (old->state == ResponseState::Submitting || old->state == ResponseState::UnknownAfterTimeout) {
      if (old->transaction_id.empty()) return ServeResult::WaitingReconciliation;
      if (effects.reconcile(old->transaction_id)) {
        (void)journal_.transition(id, ResponseState::RpcAcknowledged);
        return ServeResult::Duplicate;
      }
      return ServeResult::WaitingReconciliation;
    }
    if (old->state == ResponseState::ChainObserved) return ServeResult::Duplicate;
    // A failed send is not a served obligation. Restart must not turn a known
    // rejection into the same successful duplicate result used for an ACK.
    if (old->state == ResponseState::RpcRejected || old->state == ResponseState::Failed)
      return ServeResult::Failed;
    if (old->state == ResponseState::ResponseReorged) {
      if (old->transaction_id.empty()) return ServeResult::WaitingReconciliation;
      if (effects.reconcile(old->transaction_id)) return ServeResult::WaitingReconciliation;
      if (!journal_.reset_pre_submission(id)) return ServeResult::WaitingReconciliation;
      reset_done = true;
    }
    if(old->state==ResponseState::NotAssigned&&assignment==Assignment::PrivateRecipient&&old->transaction_id.empty()){
      if(!journal_.reconsider_unsubmitted_not_assigned(id))return ServeResult::WaitingReconciliation;
      reset_done=true;
    }
    if (terminal(old->state)&&!reset_done) return ServeResult::Duplicate;
    if (!reset_done && !journal_.reset_pre_submission(id)) return ServeResult::WaitingReconciliation;
  }

  if (assignment == Assignment::NotAssigned) {
    (void)journal_.transition(id, ResponseState::NotAssigned); return ServeResult::NotAssigned;
  }
  if (assignment == Assignment::Invalid || !journal_.transition(id, ResponseState::Assigned)) {
    (void)journal_.transition(id, ResponseState::Failed); return ServeResult::Failed;
  }
  if (effects.assigned) effects.assigned(id);
  const auto expiry = deadline.value_or(parsed->observed_daa > UINT64_MAX - stock_response_window_daa
      ? UINT64_MAX : parsed->observed_daa + stock_response_window_daa);
  auto now_daa=[&] {
    auto value=effects.current_daa?effects.current_daa():current_daa.value_or(daa);
    return std::max(value,daa);
  };
  if (now_daa() > expiry) { (void)journal_.transition(id, ResponseState::Expired); return ServeResult::Expired; }
  auto payload = decode_hex(tx.payload());
  if (!payload || !journal_.transition(id, ResponseState::RequestValidated)) {
    (void)journal_.transition(id, ResponseState::Failed); return ServeResult::Failed;
  }
  std::vector<std::uint8_t> prompt_bytes; std::array<std::uint8_t, 32> root{}; bool is_private = false;
  const int open_rc = measured_phase(effects,"request_open",[&] {
    return stock_open_request(*payload, config_.escrow_secret, prompt_bytes, root, is_private);
  });
  if (open_rc == 1) { (void)journal_.transition(id, ResponseState::NotAssigned); return ServeResult::NotAssigned; }
  if (open_rc != 0 || is_private != parsed->is_private) { (void)journal_.transition(id, ResponseState::Failed); return ServeResult::Failed; }
  std::string prompt;
  try { prompt = is_private ? prompt_from_private(prompt_bytes) : std::string(prompt_bytes.begin(), prompt_bytes.end()); }
  catch (...) { (void)journal_.transition(id, ResponseState::Failed); return ServeResult::Failed; }
  (void)journal_.transition(id, ResponseState::Inferencing);
  std::string answer;
  try { answer = measured_phase(effects,"inference",[&] {return effects.infer(prompt, parsed->max_tokens);}); }
  catch (...) { (void)journal_.transition(id, ResponseState::Failed); return ServeResult::Failed; }
  // Official miner 4ebf4bc: cap AiRequest answer bytes on a UTF-8 boundary.
  // Apply before publishing/signing so CID, word count and signed body remain consistent.
  const auto cap = nexus_ai_max_answer_len(parsed->max_tokens);
  if (answer.size() > cap) {
    auto end = static_cast<std::size_t>(cap);
    while (end > 0 && (static_cast<unsigned char>(answer[end]) & 0xc0U) == 0x80U) --end;
    answer.resize(end);
  }
  if (answer.empty()) { (void)journal_.transition(id, ResponseState::Failed); return ServeResult::Failed; }
  const auto response_daa=now_daa();
  if (response_daa > expiry) { (void)journal_.transition(id, ResponseState::Expired); return ServeResult::Expired; }
  (void)journal_.transition(id, ResponseState::ResponsePrepared);
  const std::span<const std::uint8_t> answer_bytes(reinterpret_cast<const std::uint8_t *>(answer.data()), answer.size());
  std::vector<std::uint8_t> private_body;
  std::array<std::uint8_t, 34> cid{};
  const bool inline_private = is_private && response_daa >= config_.private_inline_activation_daa;
  try {
    if (is_private) {
      private_body = measured_phase(effects,"private_seal",[&] {
        return stock_seal_private_response(root, parsed->request_hash, config_.escrow_secret, answer_bytes);
      });
      if (inline_private) cid = sha256_multihash(private_body);
      else cid = measured_phase(effects,"ipfs_publish_readback",[&] {return effects.publish_and_readback(private_body);});
    } else {
      cid = measured_phase(effects,"ipfs_publish_readback",[&] {return effects.publish_and_readback(answer_bytes);});
    }
  } catch (...) { (void)journal_.transition(id, ResponseState::Failed); return ServeResult::Failed; }
  if (response_daa >= config_.signed_response_activation_daa) {
    auto signed_payload = measured_phase(effects,"response_sign",[&] {
      return stock_build_signed_response(parsed->request_hash, config_.escrow_secret, cid, response_daa,
          answer_bytes, inline_private ? std::span<const std::uint8_t>(private_body) : std::span<const std::uint8_t>{});
    });
    std::vector<std::uint8_t> independently_opened;
    const bool response_valid = measured_phase(effects,"response_verify",[&] {
      return stock_verify_response(signed_payload, parsed->request_hash,
          config_.escrow_pubkey, inline_private ? std::optional(root) : std::nullopt,
          inline_private ? &independently_opened : nullptr);
    });
    if (!response_valid || (inline_private && !std::equal(answer_bytes.begin(), answer_bytes.end(),
                                                           independently_opened.begin(), independently_opened.end()))) {
      (void)journal_.transition(id, ResponseState::Failed); return ServeResult::Failed;
    }
    (void)journal_.transition(id, ResponseState::ResponseSigned);
    (void)journal_.transition(id, ResponseState::ResponsePublished);
    protowire::RpcTransaction response;
    response.set_version(0); response.set_locktime(0); response.set_subnetworkid("0400000000000000000000000000000000000000");
    response.set_gas(0); response.set_payload(encode_hex(signed_payload));
    if (!journal_.transition(id, ResponseState::TransactionPrepared)) { (void)journal_.transition(id, ResponseState::Failed); return ServeResult::Failed; }
    const auto txid = measured_phase(effects,"transaction_build",[&] {return official_txid(response);});
    if(now_daa()>expiry){(void)journal_.transition(id,ResponseState::Expired);return ServeResult::Expired;}
    (void)journal_.transition(id, ResponseState::Submitting, txid);
    // The node may see a DAG request before its private-request admission index is
    // populated. Stock retains the signed transaction and retries at 3 s, at most
    // 100 times. Retry ONLY that exact policy error for our verified private body.
    // SUBMITTING remains durable throughout: a crash must reconcile this txid,
    // never infer/sign another answer. No GPU allocation is held by this loop.
    const auto pending_context_error = "the AiResponse for request " +
        hex_encode(parsed->request_hash.data(), parsed->request_hash.size()) +
        " carries an inline body, but that is not a pending private request naming this responder";
    constexpr std::uint32_t max_attempts = 100;
    for (std::uint32_t attempt = 1; attempt <= max_attempts; ++attempt) {
      if (now_daa() > expiry) {
        (void)journal_.transition(id, ResponseState::Expired); return ServeResult::Expired;
      }
      const auto result = measured_phase(effects,"submit_rpc",[&] {return effects.submit(response, txid);});
      const bool retryable = inline_private &&
          result.state == AiResponseEffects::SubmitResult::State::Rejected &&
          (result.detail == pending_context_error ||
           result.detail == "Rejected transaction " + txid + ": " + pending_context_error);
      if (effects.submission_diagnostic) {
        effects.submission_diagnostic(txid, attempt,
            result.state == AiResponseEffects::SubmitResult::State::Acknowledged ? "RPC_ACKNOWLEDGED" :
            result.state == AiResponseEffects::SubmitResult::State::Unknown ? "RPC_OUTCOME_UNKNOWN" :
            retryable ? "PRIVATE_REQUEST_CONTEXT_PENDING" : "RPC_REJECTED_PERMANENT_OR_UNCLASSIFIED",
            sha256_hex(result.detail));
      }
      if (result.state == AiResponseEffects::SubmitResult::State::Acknowledged) {
        (void)journal_.transition(id, ResponseState::RpcAcknowledged); return ServeResult::Submitted;
      }
      if (result.state == AiResponseEffects::SubmitResult::State::Unknown) {
        (void)journal_.transition(id, ResponseState::UnknownAfterTimeout);
        return ServeResult::WaitingReconciliation;
      }
      if (!retryable || attempt == max_attempts || !effects.wait_before_retry) {
        (void)journal_.transition(id, ResponseState::RpcRejected); return ServeResult::Failed;
      }
      if (!measured_phase(effects,"admission_retry_wait",[&] {return effects.wait_before_retry(3000);})) {
        // Preserve the last transaction identity on cooperative cancellation.
        // Startup reconciliation must resolve it before any new side effect.
        return ServeResult::WaitingReconciliation;
      }
    }
  }
  // The pre-service-bond era response is unsigned v1 in stock. It is intentionally not enabled
  // by this service adapter: this worker supports the current signed service era only.
  (void)journal_.transition(id, ResponseState::Failed);
  return ServeResult::Failed;
}

std::array<std::uint8_t, 34> ipfs_publish_and_readback(std::string_view api_url,
    std::span<const std::uint8_t> body) {
  static const int initialized = [] { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
  if(initialized!=CURLE_OK)throw std::runtime_error("curl global initialization failed");
  const auto publication_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
  // Validate the complete URL, not a prefix which could contain userinfo or
  // a different authority. Never inherit proxy settings for the loopback API.
  std::unique_ptr<CURLU,decltype(&curl_url_cleanup)> url_parts(curl_url(),curl_url_cleanup);
  const std::string base(api_url);
  if(!url_parts || curl_url_set(url_parts.get(),CURLUPART_URL,base.c_str(),0)!=CURLUE_OK)
    throw std::invalid_argument("invalid local IPFS API URL");
  const auto part=[&](CURLUPart field) {
    char *raw=nullptr;const auto rc=curl_url_get(url_parts.get(),field,&raw,0);
    std::unique_ptr<char,decltype(&curl_free)> text(raw,curl_free);
    return rc==CURLUE_OK&&raw?std::string(raw):std::string{};
  };
  const auto port=part(CURLUPART_PORT),path=part(CURLUPART_PATH);
  if(part(CURLUPART_SCHEME)!="http" || part(CURLUPART_HOST)!="127.0.0.1" || port.empty() ||
     !part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty() ||
     !part(CURLUPART_QUERY).empty() || !part(CURLUPART_FRAGMENT).empty() ||
     (path!="/"&&!path.empty()) || base.find_first_of("\r\n")!=std::string::npos)
    throw std::invalid_argument("IPFS API must be explicitly configured on numeric loopback");
  // One handle per calling thread; reset options while retaining its bounded
  // connection cache. No handle or response buffer is shared across requests.
  thread_local std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> session(curl_easy_init(),curl_easy_cleanup);
  if(!session)throw std::runtime_error("curl handle allocation failed");
  CURL *curl=session.get();
  const auto configure=[&](const std::string &url,CurlBuffer &buffer) {
    curl_easy_reset(curl);
    curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
    curl_easy_setopt(curl,CURLOPT_POST,1L);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,"");
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,curl_write);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&buffer);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,2000L);
    const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(
        publication_deadline-std::chrono::steady_clock::now()).count();
    if(remaining<=0)throw std::runtime_error("IPFS publication budget exhausted");
    curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,std::min<long>(15000L,remaining));
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_NOPROXY,"*");
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_MAXCONNECTS,2L);
  };
  const std::string origin="http://127.0.0.1:"+port;
  CurlBuffer response;response.limit=16384;
  const auto url=origin+"/api/v0/add?pin=true&quieter=true";
  configure(url,response);
  std::unique_ptr<curl_mime,decltype(&curl_mime_free)> mime(curl_mime_init(curl),curl_mime_free);
  if(!mime)throw std::runtime_error("IPFS upload allocation failed");
  auto *mime_part=curl_mime_addpart(mime.get());
  if(!mime_part || curl_mime_name(mime_part,"file")!=CURLE_OK ||
     curl_mime_filename(mime_part,"answer.bin")!=CURLE_OK ||
     curl_mime_data(mime_part,reinterpret_cast<const char *>(body.data()),body.size())!=CURLE_OK)
    throw std::runtime_error("IPFS upload construction failed");
  curl_easy_setopt(curl,CURLOPT_MIMEPOST,mime.get());
  const auto rc=curl_easy_perform(curl);
  long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
  curl_easy_reset(curl); // Detach MIME and stack buffers before their destruction.
  mime.reset();
  if(rc!=CURLE_OK || status<200 || status>=300)throw std::runtime_error("IPFS add request failed");
  const auto key = response.bytes.find("\"Hash\"");
  if (key == std::string::npos) throw std::runtime_error("IPFS add response omitted CID");
  const auto colon = response.bytes.find(':', key); const auto quote = response.bytes.find('"', colon + 1);
  const auto end = response.bytes.find('"', quote + 1);
  if (quote == std::string::npos || end == std::string::npos) throw std::runtime_error("IPFS add CID malformed");
  const auto cid_text = response.bytes.substr(quote + 1, end - quote - 1);
  if(cid_text.size()!=46)throw std::runtime_error("IPFS CIDv0 length invalid");
  // Decode CIDv0 base58 with a bounded integer accumulator.
  static constexpr std::string_view alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  std::vector<std::uint8_t> decoded(1, 0);
  for (const char ch : cid_text) {
    const auto at = alphabet.find(ch); if (at == std::string_view::npos) throw std::runtime_error("IPFS returned invalid CIDv0");
    unsigned carry = static_cast<unsigned>(at);
    for (auto &byte : decoded) { carry += static_cast<unsigned>(byte) * 58U; byte = static_cast<std::uint8_t>(carry); carry >>= 8U; }
    while (carry) { decoded.push_back(static_cast<std::uint8_t>(carry)); carry >>= 8U; }
  }
  // Only a leading base58 '1' encodes a zero byte; internal '1' digits do not.
  const auto first_nonzero = cid_text.find_first_not_of('1');
  const auto leading_zeroes = first_nonzero == std::string::npos ? cid_text.size() : first_nonzero;
  std::vector<std::uint8_t> multihash(leading_zeroes, 0);
  for (auto it = decoded.rbegin(); it != decoded.rend(); ++it) multihash.push_back(*it);
  if (multihash.size() != 34 || multihash[0] != 0x12 || multihash[1] != 0x20)
    throw std::runtime_error("IPFS returned a non-sha256 CIDv0");
  std::array<std::uint8_t, 34> cid{}; std::copy(multihash.begin(), multihash.end(), cid.begin());
  const auto read_url=origin+"/api/v0/cat?arg="+cid_text;
  CurlBuffer readback;readback.limit=body.size();
  configure(read_url,readback);
  const auto read_rc=curl_easy_perform(curl);status=0;
  curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);curl_easy_reset(curl);
  if(read_rc!=CURLE_OK || status<200 || status>=300 || readback.bytes.size()!=body.size() ||
     !std::equal(body.begin(),body.end(),reinterpret_cast<const std::uint8_t *>(readback.bytes.data())))
    throw std::runtime_error("IPFS readback did not match published response bytes");
  // A CIDv0 hashes its DAG-PB root, not the raw file. Authenticate that actual
  // root block as well as the independently read-back file bytes.
  const auto block_url=origin+"/api/v0/block/get?arg="+cid_text;
  CurlBuffer root_block;
  configure(block_url,root_block);
  const auto block_rc=curl_easy_perform(curl);status=0;
  curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);curl_easy_reset(curl);
  if(block_rc!=CURLE_OK || status<200 || status>=300 || root_block.bytes.empty() ||
     sha256_multihash(std::span<const std::uint8_t>(
       reinterpret_cast<const std::uint8_t *>(root_block.bytes.data()),root_block.bytes.size()))!=cid)
    throw std::runtime_error("IPFS root block digest does not match CID");
  return cid;
}

struct AiResponseTransport::Impl {
  std::shared_ptr<grpc::Channel> channel;
  explicit Impl(std::string_view endpoint) {
    const auto [target,tls]=endpoint_target(endpoint);
    channel=grpc::CreateChannel(target,tls?grpc::SslCredentials(grpc::SslCredentialsOptions{}):grpc::InsecureChannelCredentials());
  }
};
AiResponseTransport::AiResponseTransport(std::string_view endpoint):impl_(std::make_unique<Impl>(endpoint)){}
AiResponseTransport::~AiResponseTransport()=default;
AiResponseEffects::SubmitResult submit_ai_response_transaction(std::string_view endpoint,
    const protowire::RpcTransaction &transaction,std::string_view expected_txid) {
  return AiResponseTransport(endpoint).submit(transaction,expected_txid);
}
AiResponseEffects::SubmitResult AiResponseTransport::submit(
    const protowire::RpcTransaction &transaction,std::string_view expected_txid) {
  const auto &channel=impl_->channel;
  const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(8);
  if (!channel->WaitForConnected(deadline)) return {AiResponseEffects::SubmitResult::State::Unknown, "connect timeout"};
  auto stub = protowire::RPC::NewStub(channel); grpc::ClientContext context; context.set_deadline(deadline);
  auto stream = stub->MessageStream(&context);
  if (!stream) return {AiResponseEffects::SubmitResult::State::Unknown, "stream creation failed"};
  protowire::KaspadRequest request; request.set_id(1); request.mutable_submittransactionrequest()->mutable_transaction()->CopyFrom(transaction);
  if (!stream->Write(request) || !stream->WritesDone()) { context.TryCancel(); return {AiResponseEffects::SubmitResult::State::Unknown, "write outcome unknown"}; }
  protowire::KaspadResponse response;
  if (!stream->Read(&response) || response.id() != request.id() || !response.has_submittransactionresponse()) {
    const auto status = stream->Finish();
    return {AiResponseEffects::SubmitResult::State::Unknown,
      status.error_message().empty() ? "response unavailable" : status.error_message()};
  }
  const auto status = stream->Finish();
  if (!status.ok()) return {AiResponseEffects::SubmitResult::State::Unknown, status.error_message()};
  const auto &result = response.submittransactionresponse();
  if (result.has_error()) return {AiResponseEffects::SubmitResult::State::Rejected, result.error().message()};
  if (result.transactionid().empty() || result.transactionid() != expected_txid)
    return {AiResponseEffects::SubmitResult::State::Unknown, "node response transaction ID mismatch or absent"};
  return {AiResponseEffects::SubmitResult::State::Acknowledged, "mempool acknowledged; chain inclusion not implied"};
}

bool ai_response_transaction_known(std::string_view endpoint,std::string_view txid) {
  return AiResponseTransport(endpoint).known(txid);
}
bool AiResponseTransport::known(std::string_view txid) {
  if (txid.size()!=64 || !decode_hex(txid))return false;
  const auto &channel=impl_->channel;
  const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
  if (!channel->WaitForConnected(deadline)) return false;
  auto stub = protowire::RPC::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(deadline);
  auto stream = stub->MessageStream(&context);
  if (!stream) return false;
  protowire::KaspadRequest request;
  request.set_id(1);
  request.mutable_getmempoolentryrequest()->set_txid(std::string(txid));
  request.mutable_getmempoolentryrequest()->set_includeorphanpool(true);
  request.mutable_getmempoolentryrequest()->set_filtertransactionpool(false);
  if (!stream->Write(request) || !stream->WritesDone()) { context.TryCancel(); return false; }
  protowire::KaspadResponse response;
  if (!stream->Read(&response) || response.id() != request.id() || !response.has_getmempoolentryresponse()) {
    context.TryCancel(); (void)stream->Finish(); return false;
  }
  const auto status = stream->Finish();
  if (!status.ok()) return false;
  const auto &result = response.getmempoolentryresponse();
  if (result.has_error() || !result.has_entry() || !result.entry().has_transaction()) return false;
  try { return official_txid(result.entry().transaction()) == txid; }
  catch (...) { return false; }
}

} // namespace keryx::nexus::ai

extern "C" int nexus_ai_response_trace(const std::uint8_t*,std::size_t,std::uint8_t*);
namespace keryx::nexus::ai {
extern "C" int nexus_ai_response_trace_v2(const std::uint8_t*,std::size_t,std::uint8_t*,std::size_t,std::size_t*);
std::optional<ResponseTraceCommitmentV2> stock_response_trace_v2(std::span<const std::uint8_t> payload) {
  std::array<std::uint8_t,73> wire{};std::size_t length=0;
  if(nexus_ai_response_trace_v2(payload.data(),payload.size(),wire.data(),wire.size(),&length)!=0 ||
      (length!=41 && length!=73))return std::nullopt;
  ResponseTraceCommitmentV2 result;auto &trace=result.trace;
  std::copy_n(wire.begin(),32,trace.root.begin());
  for(unsigned i=0;i<4;++i){trace.prompt_positions|=std::uint32_t(wire[32+i])<<(8*i);trace.total_positions|=std::uint32_t(wire[36+i])<<(8*i);}
  trace.stage=wire[40];
  if(length==73){result.tokens_root.emplace();std::copy_n(wire.begin()+41,32,result.tokens_root->begin());}
  return result;
}
std::optional<ResponseTraceCommitment> stock_response_trace(std::span<const std::uint8_t> payload) {
  const auto full=stock_response_trace_v2(payload);if(!full)return std::nullopt;return full->trace;
}
}
extern "C" int nexus_ai_verify_trace_opening(const std::uint8_t*,std::size_t,const keryx::nexus::ai::TraceOpeningContext*);
extern "C" int nexus_ai_verify_trace_opening_prefix(const std::uint8_t*,std::size_t,
    const keryx::nexus::ai::TraceOpeningContext*,const keryx::nexus::ai::TracePrefixContext*);
namespace keryx::nexus::ai {
bool stock_verify_trace_opening(std::span<const std::uint8_t> payload,const TraceOpeningContext& context) {
  return nexus_ai_verify_trace_opening(payload.data(),payload.size(),&context)==0;
}
bool stock_verify_trace_opening_prefix(std::span<const std::uint8_t> payload,
    const TraceOpeningContext& context,const TracePrefixContext& prefix) {
  return nexus_ai_verify_trace_opening_prefix(payload.data(),payload.size(),&context,&prefix)==0;
}
}

extern "C" int nexus_ai_verify_trace_opening_chain(const std::uint8_t*,std::size_t,
  const keryx::nexus::ai::TraceOpeningContext*,const keryx::nexus::ai::TracePrefixContext*,
  const keryx::nexus::ai::TraceChainContext*);
namespace keryx::nexus::ai {
bool stock_verify_trace_opening_chain(std::span<const std::uint8_t> payload,
    const TraceOpeningContext& context,const TracePrefixContext& prefix,const TraceChainContext& chain) {
  return nexus_ai_verify_trace_opening_chain(payload.data(),payload.size(),&context,&prefix,&chain)==0;
}
}
