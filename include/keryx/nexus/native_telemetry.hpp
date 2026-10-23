#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace keryx::nexus {

// CPU-side accounting for completed NEXUS PoM walk work. A work unit is one
// single-nonce CUDA walk (256 model-tile transitions in PoM v4); it is not a
// cryptographic hash and is deliberately not exposed as stock-comparable MH/s.
class NativePomTelemetry final {
 public:
  struct Counters {
    std::uint64_t gpu_work_units_launched{};
    std::uint64_t gpu_work_units_completed{};
    std::uint64_t pom_steps_completed{};
    std::uint64_t pom_kernels_completed{};
    std::uint64_t nonce_attempts{};
    std::uint64_t proofs_completed{};
    std::uint64_t proofs_rust_verified{};
    std::uint64_t proofs_fresh{};
    std::uint64_t proofs_stale{};
    std::uint64_t stale_work_discarded{};
    std::uint64_t targets_satisfied{};
    std::uint64_t eligible_attempts{};
  };

  struct Rates {
    double pom_msteps_s_1s{};
    double pom_msteps_s_10s{};
    double pom_msteps_s_60s{};
    double kernels_s_1s{};
    double kernels_s_10s{};
    double kernels_s_60s{};
    double proofs_s_1s{};
    double proofs_s_10s{};
    double proofs_s_60s{};
    double stale_work_fraction{};
    bool valid_1s{};
    bool valid_10s{};
    bool valid_60s{};
    bool valid_stale_work_fraction{};
  };

  explicit NativePomTelemetry(std::uint64_t start_monotonic_ns)
      : started_ns_(start_monotonic_ns), last_sample_ns_(start_monotonic_ns) {
    history_.push_back({start_monotonic_ns, 0, 0, 0});
  }

  void work_launched(std::uint64_t count=1) noexcept {
    launched_.fetch_add(count, std::memory_order_relaxed);
    nonce_attempts_.fetch_add(count, std::memory_order_relaxed);
  }
  void work_completed(std::uint64_t pom_steps, std::uint64_t count=1, std::uint64_t kernels=1) noexcept {
    completed_.fetch_add(count, std::memory_order_relaxed);
    steps_.fetch_add(pom_steps, std::memory_order_relaxed);
    kernels_.fetch_add(kernels, std::memory_order_relaxed);
  }
  void proof_completed() noexcept { proofs_.fetch_add(1, std::memory_order_relaxed); }
  void proof_rust_verified() noexcept { verified_.fetch_add(1, std::memory_order_relaxed); }
  void proof_fresh(bool count_eligible=true) noexcept { fresh_.fetch_add(1, std::memory_order_relaxed); if(count_eligible) eligible_attempts(1); }
  void eligible_attempts(std::uint64_t count) noexcept { eligible_.fetch_add(count, std::memory_order_relaxed); }
  void proof_stale() noexcept { stale_.fetch_add(1, std::memory_order_relaxed); }
  void work_stale(std::uint64_t count=1) noexcept { stale_work_.fetch_add(count, std::memory_order_relaxed); }
  void target_satisfied() noexcept { targets_.fetch_add(1, std::memory_order_relaxed); }
  void record_cuda_times(double kernel_ms, double h2d_ms, double d2h_ms, double engine_ms) noexcept {
    kernel_ns_.store(milliseconds_to_ns(kernel_ms), std::memory_order_relaxed);
    h2d_ns_.store(milliseconds_to_ns(h2d_ms), std::memory_order_relaxed);
    d2h_ns_.store(milliseconds_to_ns(d2h_ms), std::memory_order_relaxed);
    engine_ns_.store(milliseconds_to_ns(engine_ms), std::memory_order_relaxed);
  }
  void record_proof_times(double build_ms, double rust_ms) noexcept {
    proof_ns_.store(milliseconds_to_ns(build_ms), std::memory_order_relaxed);
    rust_ns_.store(milliseconds_to_ns(rust_ms), std::memory_order_relaxed);
  }
  void record_host_times(double gather_wall_ms, double gather_cpu_ms,
                         double reference_cpu_ms, double pack_cpu_ms,
                         double merkle_wall_ms, double merkle_cpu_ms) noexcept {
    gather_wall_ns_.store(milliseconds_to_ns(gather_wall_ms), std::memory_order_relaxed);
    gather_cpu_ns_.store(milliseconds_to_ns(gather_cpu_ms), std::memory_order_relaxed);
    reference_cpu_ns_.store(milliseconds_to_ns(reference_cpu_ms), std::memory_order_relaxed);
    pack_cpu_ns_.store(milliseconds_to_ns(pack_cpu_ms), std::memory_order_relaxed);
    merkle_wall_ns_.store(milliseconds_to_ns(merkle_wall_ms), std::memory_order_relaxed);
    merkle_cpu_ns_.store(milliseconds_to_ns(merkle_cpu_ms), std::memory_order_relaxed);
  }
  void record_template_wait(std::uint64_t wait_ns) noexcept {
    template_wait_ns_.store(wait_ns, std::memory_order_relaxed);
  }
  void record_search_host(std::uint64_t preparation_ns,std::uint64_t refresh_ns) noexcept {
    search_preparation_ns_.fetch_add(preparation_ns,std::memory_order_relaxed);
    search_refresh_ns_.fetch_add(refresh_ns,std::memory_order_relaxed);
    search_host_batches_.fetch_add(1,std::memory_order_relaxed);
  }
  void set_job_invalidations(std::uint64_t count) noexcept {
    job_invalidations_.store(count, std::memory_order_relaxed);
  }

  void set_state(std::string mining, std::string inference,
                 std::string pause_reason, std::string submission) {
    std::lock_guard lock(state_mutex_);
    mining_state_ = std::move(mining);
    inference_state_ = std::move(inference);
    pause_reason_ = std::move(pause_reason);
    submission_state_ = std::move(submission);
  }
  void set_wallet(std::string payout, std::string escrow_public_key,
                  std::string service_identity, bool authorized) {
    std::lock_guard lock(state_mutex_);
    payout_address_ = std::move(payout);
    escrow_public_key_ = std::move(escrow_public_key);
    service_identity_ = std::move(service_identity);
    wallet_authorized_ = authorized;
  }
  void set_gpu_activity(std::string activity) {
    std::lock_guard lock(state_mutex_);
    gpu_activity_ = std::move(activity);
  }
  void set_pom_backend(std::string backend) {
    std::lock_guard lock(state_mutex_);
    pom_backend_ = std::move(backend);
  }

  Counters counters() const noexcept {
    return {launched_.load(std::memory_order_relaxed),
            completed_.load(std::memory_order_relaxed),
            steps_.load(std::memory_order_relaxed),
            kernels_.load(std::memory_order_relaxed),
            nonce_attempts_.load(std::memory_order_relaxed),
            proofs_.load(std::memory_order_relaxed),
            verified_.load(std::memory_order_relaxed),
            fresh_.load(std::memory_order_relaxed),
            stale_.load(std::memory_order_relaxed),
            stale_work_.load(std::memory_order_relaxed),
            targets_.load(std::memory_order_relaxed), eligible_.load(std::memory_order_relaxed)};
  }

  // Called only by the low-priority snapshot thread. Sampling is bounded to
  // 63 points, enough for a 60s window plus a two-second sample-gap tolerance.
  Rates sample_rates(std::uint64_t now_ns) {
    const auto current = counters();
    std::lock_guard lock(history_mutex_);
    const bool monotonic_valid = now_ns > last_sample_ns_;
    if (monotonic_valid) {
      history_.push_back({now_ns, current.pom_steps_completed,
                          current.pom_kernels_completed, current.proofs_fresh, current.eligible_attempts});
      last_sample_ns_ = now_ns;
    }
    while (history_.size() > 1 && now_ns >= history_[1].time_ns &&
           now_ns - history_[1].time_ns > 61'000'000'000ULL)
      history_.pop_front();
    if (!monotonic_valid) return {};
    const auto now_counters = counters();
    const bool stale_valid = now_counters.gpu_work_units_completed != 0;
    const auto stale_fraction = stale_valid
        ? static_cast<double>(now_counters.proofs_stale + now_counters.stale_work_discarded) /
              static_cast<double>(now_counters.gpu_work_units_completed)
        : 0.0;
    return {rate_locked(now_ns, 1, Metric::Steps) / 1.0e6,
            rate_locked(now_ns, 10, Metric::Steps) / 1.0e6,
            rate_locked(now_ns, 60, Metric::Steps) / 1.0e6,
            rate_locked(now_ns, 1, Metric::Kernels),
            rate_locked(now_ns, 10, Metric::Kernels),
            rate_locked(now_ns, 60, Metric::Kernels),
            rate_locked(now_ns, 1, Metric::Proofs),
            rate_locked(now_ns, 10, Metric::Proofs),
            rate_locked(now_ns, 60, Metric::Proofs),
            stale_fraction,
            available_locked(now_ns, 1),
            available_locked(now_ns, 10), available_locked(now_ns, 60), stale_valid};
  }

  std::string json_snapshot(std::uint64_t now_ns) {
    const auto c = counters();
    const auto r = sample_rates(now_ns);
    std::string mining, inference, pause, submission, payout, escrow_pubkey, service_identity, gpu_activity, pom_backend;
    bool wallet_authorized{};
    {
      std::lock_guard lock(state_mutex_);
      mining = mining_state_;
      inference = inference_state_;
      pause = pause_reason_;
      submission = submission_state_;
      payout = payout_address_;
      escrow_pubkey = escrow_public_key_;
      service_identity = service_identity_;
      gpu_activity = gpu_activity_;
      pom_backend = pom_backend_;
      wallet_authorized = wallet_authorized_;
    }
    const auto elapsed_ns = now_ns >= started_ns_ ? now_ns - started_ns_ : 0;
    std::ostringstream out;
    out.precision(9);
    out << "{\"event\":\"NEXUS_TELEMETRY\",\"schema_version\":1"
        << ",\"clock\":\"steady_monotonic_ns\",\"monotonic_ns\":" << now_ns
        << ",\"elapsed_ns\":" << elapsed_ns
        << ",\"search_host\":{\"batches\":" << search_host_batches_.load(std::memory_order_relaxed)
        << ",\"preparation_total_ns\":" << search_preparation_ns_.load(std::memory_order_relaxed)
        << ",\"refresh_wait_total_ns\":" << search_refresh_ns_.load(std::memory_order_relaxed) << "}"
        << ",\"counters\":{\"gpu_work_units_launched\":" << c.gpu_work_units_launched
        << ",\"gpu_work_units_completed\":" << c.gpu_work_units_completed
        << ",\"pom_steps_completed\":" << c.pom_steps_completed
        << ",\"pom_kernels_completed\":" << c.pom_kernels_completed
        << ",\"nonce_attempts\":" << c.nonce_attempts
        << ",\"proofs_completed\":" << c.proofs_completed
        << ",\"proofs_rust_verified\":" << c.proofs_rust_verified
        << ",\"proofs_fresh\":" << c.proofs_fresh
        << ",\"proofs_stale\":" << c.proofs_stale
        << ",\"stale_work_discarded\":" << c.stale_work_discarded
        << ",\"targets_satisfied\":" << c.targets_satisfied
        << ",\"eligible_attempts\":" << c.eligible_attempts << "}"
        << ",\"eligible_rates\":{\"unit\":\"fresh_distinct_completed_nonces_per_second\",\"1s\":" << eligible_rate(now_ns,1)
        << ",\"10s\":" << eligible_rate(now_ns,10) << ",\"60s\":" << eligible_rate(now_ns,60) << "}"
        << ",\"rates\":{\"unit\":\"million_PoM_model_tile_steps_per_second\""
        << ",\"pom_msteps_s_1s\":" << r.pom_msteps_s_1s
        << ",\"pom_msteps_s_10s\":" << r.pom_msteps_s_10s
        << ",\"pom_msteps_s_60s\":" << r.pom_msteps_s_60s
        << ",\"valid_1s\":" << (r.valid_1s ? "true" : "false")
        << ",\"valid_10s\":" << (r.valid_10s ? "true" : "false")
        << ",\"valid_60s\":" << (r.valid_60s ? "true" : "false")
        << ",\"kernels_s_1s\":" << r.kernels_s_1s
        << ",\"kernels_s_10s\":" << r.kernels_s_10s
        << ",\"kernels_s_60s\":" << r.kernels_s_60s
        << ",\"proofs_s_1s\":" << r.proofs_s_1s
        << ",\"proofs_s_10s\":" << r.proofs_s_10s
        << ",\"proofs_s_60s\":" << r.proofs_s_60s
        << ",\"stale_work_fraction\":" << r.stale_work_fraction
        << ",\"valid_stale_work_fraction\":" << (r.valid_stale_work_fraction ? "true" : "false")
        << ",\"stock_comparable_mhs\":null"
        << ",\"stock_comparable_reason\":\"v0.5.7 packaged miner is binary-only here; its exact counter unit is not source-verifiable\"}"
        << ",\"latency_ms\":{\"cuda_kernel\":" << optional_ms(kernel_ns_.load(std::memory_order_relaxed))
        << ",\"cuda_h2d\":" << optional_ms(h2d_ns_.load(std::memory_order_relaxed))
        << ",\"cuda_d2h\":" << optional_ms(d2h_ns_.load(std::memory_order_relaxed))
        << ",\"cuda_engine_total\":" << optional_ms(engine_ns_.load(std::memory_order_relaxed))
        << ",\"proof_build\":" << optional_ms(proof_ns_.load(std::memory_order_relaxed))
        << ",\"rust_verify\":" << optional_ms(rust_ns_.load(std::memory_order_relaxed))
        << ",\"gguf_gather_wall\":" << optional_ms(gather_wall_ns_.load(std::memory_order_relaxed))
        << ",\"gguf_gather_cpu\":" << optional_ms(gather_cpu_ns_.load(std::memory_order_relaxed))
        << ",\"cpu_reference\":" << optional_ms(reference_cpu_ns_.load(std::memory_order_relaxed))
        << ",\"host_pack\":" << optional_ms(pack_cpu_ns_.load(std::memory_order_relaxed))
        << ",\"merkle_wall\":" << optional_ms(merkle_wall_ns_.load(std::memory_order_relaxed))
        << ",\"merkle_cpu\":" << optional_ms(merkle_cpu_ns_.load(std::memory_order_relaxed))
        << ",\"template_wait\":" << optional_ms(template_wait_ns_.load(std::memory_order_relaxed)) << "}"
        << ",\"scheduler\":{\"job_invalidations\":" << job_invalidations_.load(std::memory_order_relaxed) << "}"
        << ",\"state\":{\"mining\":\"" << escape_json(mining)
        << "\",\"inference\":\"" << escape_json(inference)
        << "\",\"gpu_activity\":\"" << escape_json(gpu_activity)
        << "\",\"pom_backend\":\"" << escape_json(pom_backend)
        << "\",\"pause_reason\":\"" << escape_json(pause)
        << "\",\"submission\":\"" << escape_json(submission) << "\"}"
        << ",\"wallet\":{\"authorization\":\"" << (wallet_authorized ? "AUTHORIZED" : "UNKNOWN")
        << "\",\"verification\":\"" << (wallet_authorized ? "VALID" : "UNKNOWN")
        << "\",\"payout_address\":\"" << escape_json(payout)
        << "\",\"escrow_public_key\":\"" << escape_json(escrow_pubkey)
        << "\",\"service_identity\":\"" << escape_json(service_identity)
        << "\",\"certificate_source\":\"worker_verified\",\"error\":null}}";
    return out.str();
  }

 private:
  double eligible_rate(std::uint64_t now,std::uint64_t seconds) const {
    std::lock_guard lock(history_mutex_);return rate_locked(now,seconds,Metric::Eligible);
  }
  enum class Metric { Steps, Kernels, Proofs, Eligible };
  struct Sample { std::uint64_t time_ns; std::uint64_t steps; std::uint64_t kernels; std::uint64_t proofs; std::uint64_t eligible{}; };
  static std::string escape_json(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
      if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
      else if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
    }
    return out;
  }
  static std::uint64_t milliseconds_to_ns(double milliseconds) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds <= 0.0 ||
        milliseconds >= static_cast<double>(std::numeric_limits<std::uint64_t>::max()) / 1.0e6)
      return 0;
    return static_cast<std::uint64_t>(milliseconds * 1.0e6 + 0.5);
  }
  static std::string optional_ms(std::uint64_t nanoseconds) {
    if (nanoseconds == 0) return "null";
    std::ostringstream out;
    out.precision(6);
    out << static_cast<double>(nanoseconds) / 1.0e6;
    return out.str();
  }
  bool available_locked(std::uint64_t now_ns, std::uint64_t seconds) const {
    if (now_ns < started_ns_ || now_ns - started_ns_ < seconds * 1'000'000'000ULL ||
        history_.size() < 2) return false;
    const auto cutoff = now_ns - seconds * 1'000'000'000ULL;
    const Sample *base = nullptr;
    for (const auto &sample : history_) {
      if (sample.time_ns <= cutoff) base = &sample;
      else break;
    }
    return base != nullptr && cutoff - base->time_ns <= 2'000'000'000ULL;
  }
  double rate_locked(std::uint64_t now_ns, std::uint64_t seconds, Metric metric) const {
    if (!available_locked(now_ns, seconds)) return 0.0;
    const auto cutoff = now_ns - seconds * 1'000'000'000ULL;
    const Sample *base = nullptr;
    for (const auto &sample : history_) {
      if (sample.time_ns <= cutoff) base = &sample;
      else break;
    }
    if (base == nullptr || now_ns <= base->time_ns) return 0.0;
    const auto current = counters();
    const auto base_value = metric == Metric::Steps ? base->steps :
        (metric == Metric::Kernels ? base->kernels : (metric==Metric::Eligible ? base->eligible : base->proofs));
    const auto end_value = metric == Metric::Steps ? current.pom_steps_completed :
        (metric == Metric::Kernels ? current.pom_kernels_completed : (metric==Metric::Eligible ? current.eligible_attempts : current.proofs_fresh));
    if (end_value < base_value) return 0.0;
    const auto elapsed = static_cast<double>(now_ns - base->time_ns) / 1.0e9;
    return static_cast<double>(end_value - base_value) / elapsed;
  }

  const std::uint64_t started_ns_;
  std::uint64_t last_sample_ns_;
  std::atomic<std::uint64_t> launched_{0}, completed_{0}, steps_{0}, kernels_{0};
  std::atomic<std::uint64_t> nonce_attempts_{0}, proofs_{0}, verified_{0}, fresh_{0}, stale_{0}, stale_work_{0}, targets_{0}, eligible_{0};
  std::atomic<std::uint64_t> kernel_ns_{0}, h2d_ns_{0}, d2h_ns_{0}, engine_ns_{0}, proof_ns_{0}, rust_ns_{0};
  std::atomic<std::uint64_t> gather_wall_ns_{0}, gather_cpu_ns_{0}, reference_cpu_ns_{0}, pack_cpu_ns_{0};
  std::atomic<std::uint64_t> merkle_wall_ns_{0}, merkle_cpu_ns_{0}, template_wait_ns_{0}, job_invalidations_{0};
  std::atomic<std::uint64_t> search_host_batches_{0}, search_preparation_ns_{0}, search_refresh_ns_{0};
  mutable std::mutex history_mutex_, state_mutex_;
  std::deque<Sample> history_;
  std::string mining_state_{"STARTING"}, inference_state_{"IDLE"}, pause_reason_{""}, submission_state_{"UNKNOWN"};
  std::string gpu_activity_{"WAITING"};
  std::string pom_backend_{"UNKNOWN"};
  std::string payout_address_{}, escrow_public_key_{}, service_identity_{};
  bool wallet_authorized_{};
};

}  // namespace keryx::nexus
