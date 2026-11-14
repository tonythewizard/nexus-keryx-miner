#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "keryx/nexus/pool_telemetry.hpp"
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/consensus/pom_v5.hpp"
#include "rpc.pb.h"

namespace keryx::nexus::network { class SubmissionLedger; }

namespace keryx::nexus::stratum {

using Hash = consensus::Hash;

enum class ConnectionState : std::uint8_t {
  Disconnected = 0,
  Connecting = 1,
  Connected = 2,
  Subscribed = 3,
  Authorized = 4,
  Ready = 5,
  Reconnecting = 6,
  Error = 7,
  AuthorizationRejected = 8,
};

std::string_view to_string(ConnectionState state) noexcept;

enum class ShareRejectCode : int {
  None = 0,
  Other = 20,
  JobNotFoundStale = 21,
  DuplicateShare = 22,
  LowDifficulty = 23,
  Unauthorized = 24,
  NotSubscribed = 25,
  TaskRootRequired = 47,
};

struct StratumEndpoint {
  std::string raw_url;
  std::string scheme{"stratum+tcp"};
  std::string host;
  std::uint16_t port{0};
  bool tls{false};
};

StratumEndpoint parse_stratum_endpoint(std::string_view url);
bool is_stratum_endpoint_url(std::string_view url) noexcept;
bool is_public_trace_ip(std::string_view address);

struct StratumClientConfig {
  std::string pool_url;
  std::vector<std::string> backup_urls;
  std::string worker;
  std::string password{"x"};
  std::string user_agent{nexus_pool_agent};
  // Opt-in by an H16-capable worker; only Suprnova's known endpoints need
  // the legacy protocol-version token. Hello still identifies Nexus itself.
  bool h16_protocol_compatible{false};
  PoolTelemetryOptions pool_telemetry;
  std::string protocol_tag{"keryx-stratum-v3"};
  std::string declared_model_id_hex{
      "9631eb996d2dc26efcab2c2ebd7a0eb28fb9f4fbddeca4bfbe0ec1fb1f2c9005"};
  bool tls_verify_peer{true};
  std::filesystem::path tls_ca_file;
  // Cross-host trace upload destinations require explicit deployment admission.
  std::vector<std::string> trace_upload_hosts;
  int connect_timeout_ms{5000};
  int io_timeout_ms{5000};
  int reconnect_initial_ms{500};
  int reconnect_max_ms{15000};
  // Production workers supply a private absolute journal; protocol-only mocks may omit it.
  std::filesystem::path submission_journal;
};

struct StratumPoolJob {
  std::uint64_t generation{0};
  std::string job_id;
  std::uint32_t protocol_version{4};
  protowire::RpcBlockHeader wire_header{};
  // v3 carries work fields only; a complete header exists only in v4 wire_header.
  consensus::Header header{};
  Hash pre_pow{};
  std::uint64_t timestamp{0};
  std::uint64_t daa_score{0};
  std::uint32_t bits{0};
  bool has_bits{false};
  Hash window_seed{};
  bool has_window_seed{false};
  Hash task_root{};
  bool has_task_root{false};
  Hash share_target{};
  Hash network_target{};
  Hash effective_target{};
  double difficulty{1.0};
  std::uint64_t nonce_fixed{0};
  std::uint64_t nonce_mask{0xffffffffffffffffULL};
  std::size_t extranonce2_hex_digits{16};
  std::string ai_task_json;
  std::chrono::steady_clock::time_point received_at{};
};

struct StratumChallenge {
  std::uint64_t sequence{0};
  std::string model_id_hex;
  std::string nonce_hex;
  std::chrono::steady_clock::time_point received_at{};
};
struct PoolAiMessage {
  std::string scope, wire;
  std::chrono::steady_clock::time_point received_at;
  std::string method;
  std::optional<std::uint64_t> request_id;
};

struct VerifiedPoolShare {
  std::uint64_t job_generation{0};
  std::string job_id;
  std::uint32_t protocol_version{4};
  std::uint64_t nonce{0};
  std::uint64_t daa_score{0};
  std::uint64_t timestamp{0};
  std::uint64_t final_state{0};
  Hash pre_pow{};
  Hash pow_value{};
  Hash effective_target{};
  Hash network_target{};
  Hash block_hash{};
  Hash window_seed{};
  bool has_window_seed{false};
  Hash task_root{};
  bool has_task_root{false};
  std::string opoi_tag_hex;
  std::string ipfs_cid;
  std::vector<std::uint8_t> pom_proof_borsh;
};

struct PendingShareRecord {
  std::uint64_t rpc_id{0};
  Hash ledger_id{};
  std::uint64_t job_generation{0};
  std::string job_id;
  std::uint64_t nonce{0};
  Hash pow_value{};
  Hash block_hash{};
  bool has_task_root{false};
  Hash task_root{};
  bool satisfies_network_target{false};
  bool was_b64{false};
  std::chrono::steady_clock::time_point sent_at{};
};

struct StratumTelemetrySnapshot {
  std::string mode{"pool"};
  bool connected{false};
  std::string state{"DISCONNECTED"};
  std::string endpoint;
  std::string worker;
  double ping_ms{0.0};
  double difficulty{0.0};
  std::uint64_t accepted_shares{0};
  std::uint64_t rejected_shares{0};
  std::uint64_t stale_shares{0};
  std::uint64_t duplicate_shares{0};
  std::uint64_t low_diff_shares{0};
  std::uint64_t blocks_submitted{0};
  std::uint64_t pending_shares{0};
  std::uint64_t jobs_received{0};
  std::uint64_t reconnect_count{0};
  std::uint64_t challenges_passed{0};
  std::string active_job_id;
  std::uint64_t active_daa_score{0};
  std::string window_seed_hex;
  std::string task_root_hex;
  std::string last_error;
  std::string last_share_status;
  std::string pool_telemetry_state;
  std::uint64_t pool_metadata_sent{}, pool_metadata_acked{};
  bool pool_trace_ready{};
};

Hash difficulty_to_target(double difficulty);
int compare_target_u256(const Hash &lhs_le, const Hash &rhs_le) noexcept;
std::string compute_stratum_opoi_tag(std::uint64_t nonce);
std::string hex_encode_bytes(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> hex_decode_bytes(std::string_view hex);
Hash parse_hash32_hex(std::string_view hex);

class StratumV4Client final {
public:
  explicit StratumV4Client(StratumClientConfig config);
  ~StratumV4Client();

  StratumV4Client(const StratumV4Client &) = delete;
  StratumV4Client &operator=(const StratumV4Client &) = delete;

  void start();
  void stop();

  // Synchronous step helpers for deterministic unit/integration tests and background loop
  bool connect_and_handshake_once();
  bool process_inbound_line(std::string_view line);

  std::optional<StratumPoolJob> current_job() const;
  bool is_job_current(std::uint64_t generation, std::string_view job_id) const;
  std::uint64_t current_generation() const noexcept {
    return job_generation_.load(std::memory_order_acquire);
  }

  std::optional<Hash> lookup_window_task_root(const Hash &window_seed) const;
  void commit_window_task_root(const Hash &window_seed, const Hash &task_root);

  std::optional<StratumChallenge> take_pending_challenge();
  bool submit_challenge_response(std::string_view model_id_hex,
                                 std::string_view nonce_hex,
                                 std::string_view result_text);

  std::optional<std::uint64_t> submit_share(const VerifiedPoolShare &share);
  void record_local_stale_drop(std::string_view job_id, std::uint64_t nonce,
                               std::string_view stage);

  StratumTelemetrySnapshot telemetry_snapshot() const;
  // Low-priority producer publishes cumulative completed walks, with no I/O.
  void update_pool_work(std::uint64_t completed,
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  void poll_pool_extensions(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  void set_pool_mining_available(bool available);
  void qualify_pool_ai(); // called only after native traced replay self-test
  std::optional<PoolAiMessage> take_pool_ai();
  bool submit_pool_ai_result(std::string_view scope,std::string_view result);
  // Declines a mining.ai_request this worker will not answer with error 46 (busy), so the pool
  // re-dispatches it at once instead of waiting for a timeout. Openings are never declined.
  bool decline_pool_ai(const PoolAiMessage &message);

  std::size_t endpoint_count() const noexcept { return endpoints_.size(); }
  std::size_t active_endpoint_index() const noexcept { return active_endpoint_index_; }
  void force_failover_for_test();

  void set_ai_inference_busy(bool busy) noexcept {
    ai_inference_busy_.store(busy, std::memory_order_release);
  }
  bool is_ai_inference_busy() const noexcept {
    return ai_inference_busy_.load(std::memory_order_acquire);
  }

  // Test hook to inspect outbound wire lines when running without a live socket
  std::vector<std::string> take_outbound_wire_log();
  void enable_loopback_test_mode(bool enabled);

  bool upload_trace_record(std::string_view task_id, std::span<const std::uint8_t> blob);
  void register_trace_upload_url(std::string_view task_id, std::string_view upload_url);

private:
  struct SocketHandle;

  void io_loop();
  void close_socket_locked();
  bool send_line_locked(const std::string &line);
  enum class MetadataWrite { Skipped, Sent, Broken };
  MetadataWrite send_metadata_locked(const std::string &line);
  bool read_line_once(std::string &out_line);
  void record_error_locked(std::string message);

  StratumClientConfig config_;
  std::deque<PoolAiMessage> pool_ai_queue_;
  std::map<std::uint64_t,std::chrono::steady_clock::time_point> pool_ai_pending_;
  std::unique_ptr<network::SubmissionLedger> submission_ledger_;
  std::vector<StratumEndpoint> endpoints_;
  std::size_t active_endpoint_index_{0};
  StratumEndpoint endpoint_;

  // Serialize lifecycle ownership: only one caller may join the I/O thread.
  std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::uint64_t> job_generation_{0};
  bool loopback_test_mode_{false};

  std::thread io_thread_;
  std::shared_ptr<SocketHandle> socket_;

  ConnectionState state_{ConnectionState::Disconnected};
  std::uint64_t next_rpc_id_{1};
  std::uint64_t subscribe_rpc_id_{0};
  std::uint64_t authorize_rpc_id_{0};
  std::chrono::steady_clock::time_point subscribe_sent_at_{};
  std::chrono::steady_clock::time_point authorize_sent_at_{};

  bool subscribed_{false}, authorized_{false}, hello_sent_{false};
  bool pool_mining_available_{true};
  bool metadata_session_enabled_{false},metadata_session_acked_{false};
  std::string hello_json_, pool_telemetry_state_{"DISABLED"};
  std::uint64_t pool_metadata_sent_{}, pool_metadata_acked_{};
  std::map<std::uint64_t,std::chrono::steady_clock::time_point> metadata_pending_;
  std::chrono::steady_clock::time_point metadata_started_{std::chrono::steady_clock::now()};
  std::chrono::steady_clock::time_point next_metadata_{}, next_declare_{};
  struct WorkSample { std::uint64_t completed{}; std::chrono::steady_clock::time_point at; };
  std::optional<WorkSample> pool_work_, pool_rate_base_;

  double current_difficulty_{1.0};
  Hash current_share_target_{};
  std::uint64_t nonce_fixed_{0};
  std::uint64_t nonce_mask_{0xffffffffffffffffULL};
  std::size_t extranonce2_hex_digits_{16};
  bool pool_supports_b64_proof_{false};
  std::atomic<bool> ai_inference_busy_{false};
  std::unordered_map<std::string, std::string> trace_upload_urls_;
  std::mutex trace_upload_mutex_;

  std::optional<StratumPoolJob> active_job_;
  std::unordered_map<std::string, std::uint64_t> job_ids_seen_;
  std::unordered_map<std::string, std::unordered_set<std::uint64_t>> submitted_nonces_by_job_;
  std::deque<std::pair<Hash, Hash>> window_task_roots_;
  std::optional<StratumChallenge> pending_challenge_;
  std::uint64_t challenge_seq_{0};

  std::map<std::uint64_t, PendingShareRecord> pending_shares_;
  std::vector<std::string> outbound_wire_log_;

  double ping_ms_{0.0};
  std::uint64_t accepted_shares_{0};
  std::uint64_t rejected_shares_{0};
  std::uint64_t stale_shares_{0};
  std::uint64_t duplicate_shares_{0};
  std::uint64_t low_diff_shares_{0};
  std::uint64_t blocks_submitted_{0};
  std::uint64_t jobs_received_{0};
  std::uint64_t reconnect_count_{0};
  std::uint64_t challenges_passed_{0};
  std::string last_error_;
  std::string last_share_status_{"NONE"};
};

} // namespace keryx::nexus::stratum
