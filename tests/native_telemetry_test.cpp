#include "keryx/nexus/native_telemetry.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using keryx::nexus::NativePomTelemetry;

static void require(bool ok, const char *message) {
  if (!ok) { std::cerr << "NATIVE_TELEMETRY=FAIL reason=" << message << '\n'; std::exit(1); }
}

int main() {
  constexpr std::uint64_t start = 1'000'000'000ULL;
  NativePomTelemetry telemetry(start);
  telemetry.set_pom_backend("vulkan-jit");
  require(telemetry.json_snapshot(start).find("\"pom_backend\":\"vulkan-jit\"") != std::string::npos,
          "every snapshot must identify the actual execution backend");
  telemetry.sample_rates(start);
  for (int i = 0; i < 1000; ++i) {
    telemetry.work_launched();
    telemetry.work_completed(256);
    telemetry.proof_completed();
    telemetry.proof_rust_verified();
    telemetry.proof_fresh();
  }
  const auto one = telemetry.sample_rates(start + 1'000'000'000ULL);
  require(one.valid_1s, "one-second rate window should be valid");
  require(one.pom_msteps_s_1s > 0.255999 && one.pom_msteps_s_1s < 0.256001,
          "1000 x 256 steps over one second must equal 0.256 Mstep/s");
  require(one.kernels_s_1s > 999.9 && one.kernels_s_1s < 1000.1,
          "kernel count must be one per completed single-nonce walk");
  require(one.proofs_s_1s > 999.9 && one.proofs_s_1s < 1000.1,
          "proof rate must follow Rust-verified fresh proofs");
  require(!one.valid_10s && !one.valid_60s, "long windows must be unavailable before their interval");

  for (int i = 0; i < 9000; ++i) {
    telemetry.work_launched(); telemetry.work_completed(256);
    telemetry.proof_completed(); telemetry.proof_rust_verified(); telemetry.proof_fresh();
  }
  const auto ten = telemetry.sample_rates(start + 10'000'000'000ULL);
  require(ten.valid_10s && !ten.valid_60s, "ten-second window should become valid independently");
  require(ten.pom_msteps_s_10s > 0.255999 && ten.pom_msteps_s_10s < 0.256001,
          "ten-second step rate must use the cumulative counter delta");
  for (int i = 0; i < 50'000; ++i) {
    telemetry.work_launched(); telemetry.work_completed(256);
    telemetry.proof_completed(); telemetry.proof_rust_verified(); telemetry.proof_fresh();
  }
  const auto sixty = telemetry.sample_rates(start + 60'000'000'000ULL);
  require(sixty.valid_60s, "sixty-second window should become valid");
  require(sixty.pom_msteps_s_60s > 0.255999 && sixty.pom_msteps_s_60s < 0.256001,
          "sixty-second step rate must use the cumulative counter delta");

  const auto counters = telemetry.counters();
  require(counters.gpu_work_units_launched == 60000 && counters.gpu_work_units_completed == 60000,
          "launch and completion counters must be cumulative");
  require(counters.nonce_attempts == 60000 && counters.pom_steps_completed == 15'360'000,
          "nonce and PoM step units must not be conflated");

  telemetry.work_launched(); // Simulates cancellation/failure after dispatch.
  telemetry.work_stale();
  const auto after_cancel = telemetry.counters();
  require(after_cancel.gpu_work_units_launched == 60001 &&
          after_cancel.gpu_work_units_completed == 60000 && after_cancel.pom_steps_completed == 15'360'000,
          "cancelled work must not be counted as completed");
  require(after_cancel.proofs_stale == 0 && after_cancel.stale_work_discarded == 1,
          "cancelled work is discarded without fabricating a completed stale proof");
  telemetry.work_launched();
  telemetry.work_completed(256);
  telemetry.proof_completed();
  telemetry.proof_stale(); // Completed proof became stale before it could be used.
  const auto after_stale_proof = telemetry.counters();
  require(after_stale_proof.gpu_work_units_completed == 60001 &&
          after_stale_proof.proofs_completed == 60001 && after_stale_proof.proofs_stale == 1,
          "a stale proof remains completed work and is counted once");
  telemetry.work_launched(); // A retry is a distinct dispatched nonce attempt.
  telemetry.work_completed(256);
  telemetry.proof_completed();
  telemetry.proof_rust_verified();
  telemetry.proof_fresh();
  telemetry.target_satisfied();
  const auto after_retry = telemetry.counters();
  require(after_retry.gpu_work_units_launched == 60003 &&
          after_retry.gpu_work_units_completed == 60002 &&
          after_retry.pom_steps_completed == 15'360'512 &&
          after_retry.proofs_completed == 60002 && after_retry.targets_satisfied == 1,
          "retry must count only its completed walk and target exactly once");
  telemetry.set_state("PAUSED", "RUNNING", "AI_REQUEST", "DISABLED");
  const auto paused_json = telemetry.json_snapshot(start + 60'500'000'000ULL);
  require(paused_json.find("\"pause_reason\":\"AI_REQUEST\"") != std::string::npos,
          "snapshot must carry AI pause state");
  telemetry.set_state("ACTIVE", "IDLE", "", "DISABLED"); // Resume preserves counters.
  telemetry.set_gpu_activity("CUDA_ACTIVE");
  telemetry.set_wallet("keryx:fixture", std::string(64, 'b'), "fixture-service", true);
  telemetry.record_host_times(20.0, 2.0, 0.5, 0.1, 12.0, 9.0);
  telemetry.record_template_wait(100'000);
  telemetry.set_job_invalidations(3);
  const auto after_resume = telemetry.counters();
  require(after_resume.pom_steps_completed == after_retry.pom_steps_completed &&
          after_resume.gpu_work_units_completed == after_retry.gpu_work_units_completed,
          "AI pause and PoM resume must not reset cumulative work");
  const auto stale_rate = telemetry.sample_rates(start + 61'000'000'000ULL);
  require(stale_rate.valid_stale_work_fraction && stale_rate.stale_work_fraction > 0.0000332 &&
          stale_rate.stale_work_fraction < 0.0000334,
          "stale work fraction must use stale candidate discards over completed work");
  const auto invalid_clock = telemetry.sample_rates(start - 1);
  require(!invalid_clock.valid_1s && !invalid_clock.valid_10s && !invalid_clock.valid_60s,
          "backwards timestamp must not produce a valid rate");
  const auto json = telemetry.json_snapshot(start + 62'000'000'000ULL);
  require(json.find("\"schema_version\":1") != std::string::npos,
          "snapshot schema must be versioned");
  require(json.find("\"stock_comparable_mhs\":null") != std::string::npos,
          "unproven stock conversion must remain explicitly unavailable");
  require(json.find("\"pause_reason\":\"\"") != std::string::npos,
          "snapshot must clear pause reason after PoM resumes");
  require(json.find("\"proofs_fresh\":60001") != std::string::npos,
          "fresh proof counter excludes a completed proof that became stale");
  require(json.find("\"gpu_activity\":\"CUDA_ACTIVE\"") != std::string::npos,
          "snapshot reports native GPU activity state");
  require(json.find("\"gguf_gather_wall\":20") != std::string::npos &&
          json.find("\"merkle_wall\":12") != std::string::npos &&
          json.find("\"template_wait\":0.1") != std::string::npos,
          "snapshot carries measured host, Merkle, and template wait phases");
  require(json.find("\"job_invalidations\":3") != std::string::npos &&
          json.find("\"authorization\":\"AUTHORIZED\"") != std::string::npos,
          "snapshot includes selected-chain invalidations and wallet verification state");
  NativePomTelemetry batch(start);
  batch.record_search_host(1000,2000);batch.record_search_host(3000,4000);
  batch.work_launched(1000);batch.work_completed(256000,1000);batch.eligible_attempts(900);batch.work_stale(100);
  const auto batch_json=batch.json_snapshot(start+2'000'000'000ULL);
  require(batch_json.find("\"search_host\":{\"batches\":2,\"preparation_total_ns\":4000,\"refresh_wait_total_ns\":6000}")!=std::string::npos,
          "ordinary search preparation and refresh times must remain distinct integer counters");
  require(batch.counters().gpu_work_units_completed==1000&&batch.counters().pom_kernels_completed==1,
          "a completed batch counts nonces independently from kernel launches");
  require(batch.counters().proofs_rust_verified==0&&batch.counters().eligible_attempts==900,
          "GPU target-tested attempts must not impersonate complete Rust verified proofs");
  require(batch_json.find("\"1s\":450")!=std::string::npos,
          "eligible rolling rate uses native 900 completed fresh nonces over two seconds");
  std::cout << "NATIVE_TELEMETRY=PASS deterministic_units=1000x256 rolling_windows=1,10,60 "
               "cancelled_not_completed=PASS stock_mhs=N/A schema=1\n";
}
