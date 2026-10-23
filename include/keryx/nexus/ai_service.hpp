#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <unordered_map>
#include <vector>

namespace protowire { class RpcTransaction; class GetServiceStrikesResponseMessage; }

namespace keryx::nexus::ai {

inline constexpr std::string_view request_subnetwork = "0300000000000000000000000000000000000000";
inline constexpr std::size_t request_header_bytes = 52;
inline constexpr std::size_t max_request_bytes = 65536;
inline constexpr std::uint64_t min_priority_fee = 30'000'000;
inline constexpr std::uint64_t stock_response_window_daa = 1000;
// Official v0.5.7 H8 request identity gate on mainnet. Testnet activates at genesis.
inline constexpr std::uint64_t mainnet_request_txid_gate_daa = 79'210'000;
inline constexpr std::uint64_t mainnet_private_inference_gate_daa = 121'985'000;
inline constexpr std::uint64_t mainnet_signed_response_activation_daa = 76'316'623;
inline constexpr std::uint64_t testnet_private_inference_gate_daa = 6'000;
inline constexpr std::uint64_t service_window_v1_base_daa = 300;
inline constexpr std::uint64_t service_window_v2_base_daa = 3'000;
inline constexpr std::size_t max_pending_requests = 64;

struct Request {
  std::array<std::uint8_t, 32> txid{};
  std::array<std::uint8_t, 32> request_hash{};
  std::array<std::uint8_t, 32> model_id{};
  std::uint32_t max_tokens{};
  std::uint64_t inference_reward{};
  std::uint64_t priority_fee{};
  std::uint64_t observed_daa{};
  std::vector<std::uint8_t> prompt_or_private_envelope;
  std::string stable_id;
  bool is_private{};
};

enum class Assignment { NotAssigned, PublicEligible, PrivateRecipient, Invalid };

// Exact node transaction-ID serialization, including claim inputs/outputs.
std::optional<std::array<std::uint8_t, 32>> consensus_transaction_id(const protowire::RpcTransaction &tx);

// Parse the stock v0.5.7 transaction payload and apply the H8 identity rule: transaction ID
// at/after activation, BLAKE2b-256(payload) before it. Missing verbose txid uses the exact
// consensus transaction serialization, never a payload-hash substitute after H8.
std::optional<Request> parse_request(const protowire::RpcTransaction &tx,
                                     std::uint64_t containing_daa,
                                     std::string *why = nullptr,
                                     std::string_view network = "mainnet");
std::optional<std::uint64_t> service_response_deadline(std::uint64_t assignment_seed_daa,
                                                       std::uint32_t tier,
                                                       std::uint32_t max_tokens,
                                                       bool service_bond_v2);
Assignment classify_assignment(const Request &request,
                               std::string_view own_service_identity_hex,
                               std::string_view own_escrow_xonly_hex,
                               std::uint32_t expected_tier,
                               const std::vector<struct ServiceProvider> &providers);

struct PendingRecord {
  std::string stable_id;
  std::string request_txid;
  std::uint64_t observed_daa{};
  std::optional<std::uint64_t> response_deadline_daa;
  std::string state; // DISCOVERED, ASSIGNED, INFERENCE, RESPONSE_SUBMITTED, CONFIRMED, EXPIRED
};

// Durable bounded journal. It stores only identifiers and lifecycle metadata; prompts,
// ciphertext, escrow keys and inference text are never written to this journal.
class RequestJournal final {
public:
  explicit RequestJournal(std::filesystem::path path);
  void recover();
  bool discover(const Request &request, bool assigned,
                std::optional<std::uint64_t> authoritative_deadline_daa = std::nullopt);
  bool transition(std::string_view stable_id, std::string_view expected,
                  std::string_view next);
  std::vector<PendingRecord> pending() const;
  std::size_t size() const;
private:
  void persist_locked(const PendingRecord &changed);
  std::filesystem::path path_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, PendingRecord> records_;
};

struct StrikeSnapshot {
  enum class Availability { Unknown, Available } availability{Availability::Unknown};
  std::string identity;
  std::optional<std::uint32_t> consecutive;
  std::optional<std::uint32_t> lifetime;
  std::optional<std::uint64_t> last_strike_daa;
  std::optional<std::uint64_t> suspended_until_daa;
  std::uint32_t pending_burns{};
  std::uint64_t pending_burn_sompi{};
  std::uint64_t virtual_daa{};
  std::uint64_t observed_unix_ms{};
  bool stale{};
  std::string error;
};

struct ServiceProvider {
  std::uint32_t tier{};
  std::string model_id;
  std::string identity;
  std::string escrow_pubkey;
};

// Derives the node's service-ledger identity for a version-0 Keryx payout address.
// Returns nullopt for unsupported address versions or invalid checksum/payload.
std::optional<std::string> service_identity_from_payout(std::string_view payout_address,
                                                        std::string *why = nullptr);

struct ServiceRpcSnapshot {
  bool providers_available{};
  bool strikes_available{};
  std::uint64_t providers_virtual_daa{};
  std::vector<ServiceProvider> providers;
  StrikeSnapshot strikes;
  std::string providers_error;
};

enum class ProviderListingStatus { Unknown, NotListed, Listed };

// GetServiceProviders is the current selected-chain eligibility cohort, not a
// pre-mining registration registry. A successful non-match is distinct from an
// unavailable RPC response.
ProviderListingStatus provider_listing_status(const ServiceRpcSnapshot &snapshot,
                                               std::uint32_t tier,
                                               std::string_view model_id,
                                               std::string_view service_identity,
                                               std::string_view escrow_pubkey);

// Match exclusively by the node's service-ledger miner identity. An unavailable identity or
// RPC error returns UNKNOWN; absence from a successful list is reported as observed zero.
StrikeSnapshot strikes_for_identity(const protowire::GetServiceStrikesResponseMessage &response,
                                    std::string_view service_identity_hex);

std::string hex_encode(const std::uint8_t *data, std::size_t size);

} // namespace keryx::nexus::ai
