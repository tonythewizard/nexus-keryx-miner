#include "keryx/nexus/ai_service.hpp"

#include "rpc.pb.h"
#include <blake2.h>

#include <openssl/sha.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace keryx::nexus::ai {
namespace {
constexpr std::string_view private_magic{"\0KXP", 4};
constexpr std::size_t private_header = 51;
constexpr std::size_t recipient_size = 80;
constexpr std::size_t auth_tag = 16;
constexpr std::string_view txid_key = "TransactionID";

std::optional<std::vector<std::uint8_t>> decode_hex(std::string_view in) {
  if ((in.size() & 1U) != 0) return std::nullopt;
  std::vector<std::uint8_t> out(in.size() / 2);
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = nibble(in[i * 2]);
    const int lo = nibble(in[i * 2 + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

void append_le(std::string &out, std::uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i)
    out.push_back(static_cast<char>((value >> (8U * i)) & 0xffU));
}

bool digest_blake2b(std::string_view bytes, const void *key, std::size_t key_len,
                    std::array<std::uint8_t, 32> &out) {
  return blake2b(out.data(), bytes.data(), key, out.size(), bytes.size(), key_len) == 0;
}

std::optional<std::array<std::uint8_t, 32>> official_txid(const protowire::RpcTransaction &tx) {
  std::string serialized;
  serialized.reserve(tx.payload().size() + static_cast<std::size_t>(tx.inputs_size()) * 64U +
                     static_cast<std::size_t>(tx.outputs_size()) * 64U + 96U);
  append_le(serialized, static_cast<std::uint16_t>(tx.version()), 2);
  append_le(serialized, static_cast<std::uint64_t>(tx.inputs_size()), 8);
  for (const auto &input : tx.inputs()) {
    if (!input.has_previousoutpoint()) return std::nullopt;
    const auto id = decode_hex(input.previousoutpoint().transactionid());
    if (!id || id->size() != 32) return std::nullopt;
    serialized.append(reinterpret_cast<const char *>(id->data()), id->size());
    append_le(serialized, input.previousoutpoint().index(), 4);
    append_le(serialized, 0, 8); // consensus txid excludes signatureScript
    append_le(serialized, input.sequence(), 8);
  }
  append_le(serialized, static_cast<std::uint64_t>(tx.outputs_size()), 8);
  for (const auto &output : tx.outputs()) {
    if (!output.has_scriptpublickey()) return std::nullopt;
    const auto script = decode_hex(output.scriptpublickey().scriptpublickey());
    if (!script) return std::nullopt;
    append_le(serialized, output.amount(), 8);
    append_le(serialized, output.scriptpublickey().version(), 2);
    append_le(serialized, static_cast<std::uint64_t>(script->size()), 8);
    serialized.append(reinterpret_cast<const char *>(script->data()), script->size());
  }
  append_le(serialized, tx.locktime(), 8);
  const auto subnetwork = decode_hex(tx.subnetworkid());
  if (!subnetwork || subnetwork->size() != 20) return std::nullopt;
  serialized.append(reinterpret_cast<const char *>(subnetwork->data()), subnetwork->size());
  append_le(serialized, tx.gas(), 8);
  const auto payload = decode_hex(tx.payload());
  if (!payload) return std::nullopt;
  append_le(serialized, static_cast<std::uint64_t>(payload->size()), 8);
  serialized.append(reinterpret_cast<const char *>(payload->data()), payload->size());
  std::array<std::uint8_t, 32> result{};
  if (!digest_blake2b(serialized, txid_key.data(), txid_key.size(), result)) return std::nullopt;
  return result;
}

bool private_envelope_valid(std::span<const std::uint8_t> p) {
  if (p.size() < private_header ||
      !std::equal(private_magic.begin(), private_magic.end(), p.begin()) || p[4] != 1)
    return false;
  const auto count = static_cast<std::size_t>(p[50]);
  if (count == 0 || count > 128) return false;
  const auto entries_end = private_header + count * recipient_size;
  if (p.size() < entries_end + auth_tag) return false;
  EC_GROUP *raw_group = EC_GROUP_new_by_curve_name(NID_secp256k1);
  BN_CTX *raw_ctx = BN_CTX_new();
  EC_POINT *raw_point = raw_group ? EC_POINT_new(raw_group) : nullptr;
  BIGNUM *raw_x = BN_new();
  if (!raw_group || !raw_ctx || !raw_point || !raw_x) {
    EC_POINT_free(raw_point); BN_free(raw_x); BN_CTX_free(raw_ctx); EC_GROUP_free(raw_group);
    return false;
  }
  const auto valid_xonly = [&](const std::uint8_t *key) {
    return BN_bin2bn(key, 32, raw_x) != nullptr &&
      EC_POINT_set_compressed_coordinates(raw_group, raw_point, raw_x, 0, raw_ctx) == 1 &&
      EC_POINT_is_on_curve(raw_group, raw_point, raw_ctx) == 1;
  };
  const bool ephemeral_valid = EC_POINT_oct2point(raw_group, raw_point, p.data() + 5, 33, raw_ctx) == 1 &&
      EC_POINT_is_on_curve(raw_group, raw_point, raw_ctx) == 1;
  if (!ephemeral_valid) {
    EC_POINT_free(raw_point); BN_free(raw_x); BN_CTX_free(raw_ctx); EC_GROUP_free(raw_group);
    return false;
  }
  for (std::size_t i = 1; i < count; ++i) {
    const auto *prev = p.data() + private_header + (i - 1) * recipient_size;
    const auto *curr = p.data() + private_header + i * recipient_size;
    if (!std::lexicographical_compare(prev, prev + 32, curr, curr + 32)) {
      EC_POINT_free(raw_point); BN_free(raw_x); BN_CTX_free(raw_ctx); EC_GROUP_free(raw_group);
      return false;
    }
  }
  bool keys_valid = true;
  for (std::size_t i = 0; i < count; ++i)
    keys_valid = keys_valid && valid_xonly(p.data() + private_header + i * recipient_size);
  EC_POINT_free(raw_point); BN_free(raw_x); BN_CTX_free(raw_ctx); EC_GROUP_free(raw_group);
  return keys_valid;
}

std::string sha256_hex(std::string_view bytes) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), digest.data());
  return hex_encode(digest.data(), digest.size());
}

std::string encode_record(const PendingRecord &r) {
  const std::string body = r.stable_id + "|" + r.request_txid + "|" +
      std::to_string(r.observed_daa) + "|" +
      (r.response_deadline_daa ? std::to_string(*r.response_deadline_daa) : "?") + "|" + r.state;
  return body + "|" + sha256_hex(body) + "\n";
}

std::optional<PendingRecord> decode_record(std::string_view line) {
  if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
  const auto last = line.rfind('|');
  if (last == std::string_view::npos || sha256_hex(line.substr(0, last)) != line.substr(last + 1))
    return std::nullopt;
  std::array<std::string_view, 5> parts{};
  std::size_t start = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto sep = line.find('|', start);
    if (sep == std::string_view::npos || sep >= last) return std::nullopt;
    parts[i] = line.substr(start, sep - start);
    start = sep + 1;
  }
  parts[4] = line.substr(start, last - start);
  if (parts[0].size() != 64 || parts[1].size() != 64 || parts[4].empty()) return std::nullopt;
  PendingRecord r;
  r.stable_id = parts[0]; r.request_txid = parts[1]; r.state = parts[4];
  auto parse = [](std::string_view text, std::uint64_t &value) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size();
  };
  if (!parse(parts[2], r.observed_daa)) return std::nullopt;
  if (parts[3] != "?") {
    std::uint64_t deadline{};
    if (!parse(parts[3], deadline)) return std::nullopt;
    r.response_deadline_daa = deadline;
  }
  return r;
}

void write_all(int fd, std::string_view text) {
  while (!text.empty()) {
    const auto n = ::write(fd, text.data(), text.size());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("AI request journal write failed");
    text.remove_prefix(static_cast<std::size_t>(n));
  }
}

bool terminal_state(std::string_view s) {
  return s == "CONFIRMED" || s == "EXPIRED" || s == "NOT_ASSIGNED";
}
} // namespace

std::optional<std::array<std::uint8_t, 32>> consensus_transaction_id(const protowire::RpcTransaction &tx) {
  return official_txid(tx);
}

std::string hex_encode(const std::uint8_t *data, std::size_t size) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out(size * 2, '0');
  for (std::size_t i = 0; i < size; ++i) {
    out[i * 2] = digits[data[i] >> 4]; out[i * 2 + 1] = digits[data[i] & 15U];
  }
  return out;
}

std::optional<Request> parse_request(const protowire::RpcTransaction &tx,
                                     std::uint64_t containing_daa,
                                     std::string *why,
                                     std::string_view network) {
  const auto fail = [&](std::string_view message) -> std::optional<Request> {
    if (why) *why = message;
    return std::nullopt;
  };
  if (tx.subnetworkid() != request_subnetwork) return fail("not official AiRequest subnetwork");
  const auto payload = decode_hex(tx.payload());
  if (!payload || payload->size() < request_header_bytes ||
      payload->size() > max_request_bytes) return fail("AiRequest payload length or encoding invalid");
  if (network != "mainnet" && network != "testnet") return fail("unsupported network for request identity rules");
  const bool private_era = network == "mainnet"
      ? containing_daa >= mainnet_private_inference_gate_daa
      : containing_daa >= testnet_private_inference_gate_daa;
  if (!private_era && payload->size() > 4096)
    return fail("AiRequest exceeds the official pre-private-era 4096-byte payload limit");
  std::optional<std::array<std::uint8_t, 32>> txid;
  if (tx.has_verbosedata() && tx.verbosedata().transactionid().size() == 64) {
    const auto id = decode_hex(tx.verbosedata().transactionid());
    if (id && id->size() == 32) {
      std::array<std::uint8_t, 32> parsed{};
      std::copy(id->begin(), id->end(), parsed.begin());
      txid = parsed;
    }
  }
  if (!txid) txid = official_txid(tx);
  if (!txid) return fail("transaction ID unavailable and official transaction serialization failed");
  Request r;
  r.txid = *txid;
  std::copy_n(payload->begin(), 32, r.model_id.begin());
  const auto le32 = [&](std::size_t offset) {
    return static_cast<std::uint32_t>((*payload)[offset]) |
        (static_cast<std::uint32_t>((*payload)[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>((*payload)[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>((*payload)[offset + 3]) << 24U);
  };
  const auto le64 = [&](std::size_t offset) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>((*payload)[offset + i]) << (8U * i);
    return v;
  };
  r.max_tokens = le32(32); r.inference_reward = le64(36); r.priority_fee = le64(44);
  r.observed_daa = containing_daa;
  r.prompt_or_private_envelope.assign(payload->begin() + static_cast<std::ptrdiff_t>(request_header_bytes), payload->end());
  r.is_private = r.prompt_or_private_envelope.size() >= private_magic.size() &&
      std::equal(private_magic.begin(), private_magic.end(), r.prompt_or_private_envelope.begin());
  if (r.max_tokens == 0 || r.max_tokens > 4096 || r.priority_fee < min_priority_fee)
    return fail("AiRequest parameters violate stock payload bounds");
  if (r.is_private && !private_envelope_valid(r.prompt_or_private_envelope))
    return fail("private envelope malformed");
  if (private_era && !r.is_private)
    return fail("H14 consensus requires an authenticated private request envelope");
  const bool txid_identity = network == "testnet" || containing_daa >= mainnet_request_txid_gate_daa;
  if (txid_identity) {
    r.request_hash = r.txid;
  } else if (!digest_blake2b(std::string_view(reinterpret_cast<const char *>(payload->data()), payload->size()),
                            nullptr, 0, r.request_hash)) {
    return fail("pre-H8 request payload digest failed");
  }
  r.stable_id = hex_encode(r.request_hash.data(), r.request_hash.size());
  return r;
}

std::optional<std::uint64_t> service_response_deadline(std::uint64_t seed_daa,
    std::uint32_t tier, std::uint32_t max_tokens, bool service_bond_v2) {
  if (tier > 4 || max_tokens == 0 || max_tokens > 4096) return std::nullopt;
  const std::uint64_t per_token = tier <= 2 ? 2U : (tier == 3 ? 3U : 4U);
  const std::uint64_t base = service_bond_v2 ? service_window_v2_base_daa : service_window_v1_base_daa;
  const std::uint64_t window = base + static_cast<std::uint64_t>(max_tokens) * per_token;
  if (seed_daa > std::numeric_limits<std::uint64_t>::max() - window) return std::nullopt;
  return seed_daa + window;
}

Assignment classify_assignment(const Request &request, std::string_view own_identity,
                               std::string_view own_key, std::uint32_t expected_tier,
                               const std::vector<ServiceProvider> &providers) {
  if (request.stable_id.size() != 64 || request.max_tokens == 0) return Assignment::Invalid;
  if (own_identity.size() != 64 || own_key.size() != 64) return Assignment::NotAssigned;
  const auto id_equal = [](std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
      if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
  };
  const auto expected_model = hex_encode(request.model_id.data(), request.model_id.size());
  const bool provider_match = std::any_of(providers.begin(), providers.end(), [&](const auto &provider) {
    return provider.tier == expected_tier && id_equal(provider.model_id, expected_model) &&
           id_equal(provider.identity, own_identity) && id_equal(provider.escrow_pubkey, own_key);
  });
  // Stock v0.5.7 client/grpc.rs routes PRIVATE requests by authenticated recipients,
  // independently of the node's current (time-windowed) public provider listing.
  if (!request.is_private) return provider_match ? Assignment::PublicEligible : Assignment::NotAssigned;
  const auto key = decode_hex(own_key);
  if (!key || key->size() != 32 || !private_envelope_valid(request.prompt_or_private_envelope))
    return Assignment::Invalid;
  const auto &envelope = request.prompt_or_private_envelope;
  const std::size_t count = envelope[50];
  for (std::size_t i = 0; i < count; ++i) {
    const auto *recipient = envelope.data() + private_header + i * recipient_size;
    if (std::equal(key->begin(), key->end(), recipient)) return Assignment::PrivateRecipient;
  }
  return Assignment::NotAssigned;
}

ProviderListingStatus provider_listing_status(const ServiceRpcSnapshot &snapshot,
                                               std::uint32_t tier,
                                               std::string_view model_id,
                                               std::string_view service_identity,
                                               std::string_view escrow_pubkey) {
  if (!snapshot.providers_available) return ProviderListingStatus::Unknown;
  const auto equal_ascii_ci = [](std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
  };
  const bool found = std::any_of(snapshot.providers.begin(), snapshot.providers.end(), [&](const auto &provider) {
    return provider.tier == tier && equal_ascii_ci(provider.model_id, model_id) &&
           equal_ascii_ci(provider.identity, service_identity) &&
           equal_ascii_ci(provider.escrow_pubkey, escrow_pubkey);
  });
  return found ? ProviderListingStatus::Listed : ProviderListingStatus::NotListed;
}

RequestJournal::RequestJournal(std::filesystem::path path) : path_(std::move(path)) {
  path_ = std::filesystem::absolute(path_).lexically_normal();
  if (path_.empty() || path_ == "/tmp" || path_.string().starts_with("/tmp/"))
    throw std::invalid_argument("AI request journal requires a persistent non-/tmp path");
}

void RequestJournal::recover() {
  std::lock_guard lock(mutex_);
  records_.clear();
  const auto load = [&](const std::filesystem::path &p, bool strict) {
    std::ifstream in(p);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
      auto record = decode_record(line);
      if (!record) {
        if (strict) throw std::runtime_error("AI request snapshot checksum/record invalid");
        throw std::runtime_error("AI request journal checksum/record invalid");
      }
      records_[record->stable_id] = std::move(*record);
    }
  };
  load(path_, true);
  load(path_.string() + ".journal", false);
  if (records_.size() > max_pending_requests * 100U)
    throw std::runtime_error("AI request recovery index exceeds bounded safety limit");
}

bool RequestJournal::discover(const Request &request, bool assigned,
                              std::optional<std::uint64_t> authoritative_deadline_daa) {
  std::lock_guard lock(mutex_);
  if (auto existing = records_.find(request.stable_id); existing != records_.end()) {
    return false;
  }
  if (records_.size() >= 10000)
    throw std::runtime_error("AI request deduplication index reached its bounded 10000-entry limit");
  const auto pending_count = std::count_if(records_.begin(), records_.end(), [](const auto &item) {
    return !terminal_state(item.second.state);
  });
  if (assigned && pending_count >= static_cast<std::ptrdiff_t>(max_pending_requests))
    throw std::runtime_error("bounded AI request queue is full; refusing to drop an assigned obligation");
  if (authoritative_deadline_daa && *authoritative_deadline_daa < request.observed_daa)
    throw std::invalid_argument("authoritative AiRequest deadline precedes its observed DAA");
  PendingRecord r{request.stable_id, hex_encode(request.txid.data(), request.txid.size()), request.observed_daa,
                  authoritative_deadline_daa,
                  assigned ? "ASSIGNED" : "NOT_ASSIGNED"};
  records_.emplace(r.stable_id, r);
  persist_locked(records_.at(r.stable_id));
  return true;
}

bool RequestJournal::transition(std::string_view id, std::string_view expected,
                                std::string_view next) {
  std::lock_guard lock(mutex_);
  auto it = records_.find(std::string(id));
  if (it == records_.end() || it->second.state != expected) return false;
  const bool valid_edge = (expected == "DISCOVERED" && (next == "ASSIGNED" || next == "EXPIRED")) ||
      (expected == "ASSIGNED" && (next == "INFERENCE" || next == "EXPIRED")) ||
      (expected == "INFERENCE" && (next == "RESPONSE_SUBMITTED" || next == "EXPIRED")) ||
      (expected == "RESPONSE_SUBMITTED" && (next == "CONFIRMED" || next == "EXPIRED"));
  if (!valid_edge) throw std::invalid_argument("invalid AI request lifecycle transition");
  it->second.state = next;
  persist_locked(it->second);
  return true;
}

std::vector<PendingRecord> RequestJournal::pending() const {
  std::lock_guard lock(mutex_);
  std::vector<PendingRecord> out;
  for (const auto &[_, record] : records_) if (!terminal_state(record.state)) out.push_back(record);
  std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.observed_daa < b.observed_daa; });
  return out;
}

std::size_t RequestJournal::size() const {
  std::lock_guard lock(mutex_);
  return records_.size();
}

void RequestJournal::persist_locked(const PendingRecord &changed) {
  std::filesystem::create_directories(path_.parent_path());
  const auto journal_path = std::filesystem::path(path_.string() + ".journal");
  const int jfd = ::open(journal_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (jfd < 0) throw std::runtime_error("cannot open AI request journal");
  try {
    write_all(jfd, encode_record(changed));
    if (::fsync(jfd) != 0) throw std::runtime_error("AI request journal fsync failed");
    ::close(jfd);
  } catch (...) { ::close(jfd); throw; }

  const auto next = std::filesystem::path(path_.string() + ".new");
  int fd = ::open(next.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("cannot create AI request snapshot");
  try {
    std::vector<std::string> ordered;
    ordered.reserve(records_.size());
    for (const auto &[_, rec] : records_) ordered.push_back(encode_record(rec));
    std::sort(ordered.begin(), ordered.end());
    for (const auto &line : ordered) write_all(fd, line);
    if (::fsync(fd) != 0) throw std::runtime_error("AI request snapshot fsync failed");
    ::close(fd);
    fd = -1;
    if (::rename(next.c_str(), path_.c_str()) != 0) throw std::runtime_error("AI request snapshot atomic rename failed");
    const int dfd = ::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { (void)::fsync(dfd); ::close(dfd); }
    // Snapshot is durable before log truncation. If a crash occurs between these operations,
    // replaying the old journal is idempotent because each line contains complete state.
    const int compact_fd = ::open(journal_path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (compact_fd < 0) throw std::runtime_error("cannot compact AI request journal");
    if (::fsync(compact_fd) != 0) { ::close(compact_fd); throw std::runtime_error("AI request journal compaction fsync failed"); }
    ::close(compact_fd);
    const int compact_dir = ::open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (compact_dir >= 0) { (void)::fsync(compact_dir); ::close(compact_dir); }
  } catch (...) { if (fd >= 0) ::close(fd); throw; }
}

std::optional<std::string> service_identity_from_payout(std::string_view address, std::string *why) {
  const auto fail = [&](std::string_view reason) -> std::optional<std::string> {
    if (why) *why = reason;
    return std::nullopt;
  };
  const auto colon = address.find(':');
  if (colon == std::string_view::npos || address.substr(0, colon) != "keryx")
    return fail("payout address HRP is not the official keryx prefix");
  const auto encoded = address.substr(colon + 1);
  if (encoded.size() < 9) return fail("payout address data is too short");
  const auto data = encoded.substr(0, encoded.size() - 8); // stock decoder excludes checksum bytes
  static constexpr std::string_view charset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
  std::vector<std::uint8_t> decoded;
  decoded.reserve(data.size() * 5 / 8);
  std::uint32_t buffer = 0, bits = 0;
  for (const char c : data) {
    const auto pos = charset.find(c);
    if (pos == std::string_view::npos) return fail("payout address contains an invalid bech32 data character");
    buffer = (buffer << 5U) | static_cast<std::uint32_t>(pos);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      decoded.push_back(static_cast<std::uint8_t>(buffer >> bits));
    }
  }
  if (decoded.size() < 33) return fail("payout address lacks the official 32-byte public key");
  const std::uint16_t version = decoded[0];
  if (version != 0) return fail("service identity supports only stock Schnorr P2PK address version 0");
  std::string script;
  script.reserve(34);
  script.push_back(static_cast<char>(0x20));
  script.append(reinterpret_cast<const char *>(decoded.data() + 1), 32);
  script.push_back(static_cast<char>(0xac));
  std::string preimage;
  preimage.reserve(2 + script.size());
  append_le(preimage, version, 2);
  preimage += script;
  std::array<std::uint8_t, 32> identity{};
  constexpr std::string_view key = "TransactionHash";
  if (!digest_blake2b(preimage, key.data(), key.size(), identity))
    return fail("service identity digest failed");
  return hex_encode(identity.data(), identity.size());
}

StrikeSnapshot strikes_for_identity(const protowire::GetServiceStrikesResponseMessage &response,
                                    std::string_view identity) {
  StrikeSnapshot out;
  out.identity = std::string(identity);
  if (response.has_error()) { out.error = response.error().message(); return out; }
  if (identity.size() != 64 || !decode_hex(identity)) {
    out.error = "service identity unavailable or malformed";
    return out;
  }
  out.availability = StrikeSnapshot::Availability::Available;
  out.virtual_daa = response.virtualdaascore();
  auto equal_id = [&](const std::string &candidate) {
    if (candidate.size() != identity.size()) return false;
    for (std::size_t i = 0; i < identity.size(); ++i)
      if (std::tolower(static_cast<unsigned char>(candidate[i])) !=
          std::tolower(static_cast<unsigned char>(identity[i]))) return false;
    return true;
  };
  out.consecutive = 0; out.lifetime = 0; out.pending_burns = 0; out.pending_burn_sompi = 0;
  for (const auto &item : response.strikes()) if (equal_id(item.miner())) {
    out.consecutive = item.consecutivemisses(); out.last_strike_daa = item.laststrikedaascore();
  }
  for (const auto &item : response.lifetimestrikes()) if (equal_id(item.miner())) out.lifetime = item.strikes();
  for (const auto &item : response.suspended()) if (equal_id(item.miner())) out.suspended_until_daa = item.untildaascore();
  for (const auto &item : response.pendingburns()) if (equal_id(item.miner())) {
    ++out.pending_burns; out.pending_burn_sompi += item.burnedsompi();
  }
  return out;
}

} // namespace keryx::nexus::ai
