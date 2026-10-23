#include "keryx/nexus/primary_vulkan_workload.hpp"
#include "keryx/nexus/secondary_vulkan_config.hpp"
#include "keryx/nexus/vulkan_inference_process.hpp"
#include "keryx/nexus/amd_memory_handoff.hpp"
#include "keryx/nexus/gpu_preemption.hpp"
#include "keryx/nexus/grpc_header.hpp"
#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/opoi.hpp"
#include "messages.grpc.pb.h"
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>

using namespace keryx::nexus;
using namespace keryx::nexus::consensus;

namespace {
int current_thread_count() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("Threads:", 0) == 0) {
      std::istringstream iss(line.substr(8));
      int count = 0;
      if (iss >> count) return count;
    }
  }
  throw std::runtime_error("Could not read Threads from /proc/self/status");
}

void require(bool ok, const char *msg) {
  if (!ok) throw std::runtime_error(msg);
}

struct Lease {
  int fd{-1};
  ~Lease() {
    if (fd >= 0) close(fd);
  }
};
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 8) {
      std::cerr << "Usage: vulkan_pipeline_lifecycle_test MODEL TREE ORACLE MODULE SHA CAPTURE IDENTITY [DEVICE] [HELPER HELPER_SHA LIB LIB_SHA LEASE_LOCK]\n";
      return 1;
    }
    const std::string model_path = argv[1];
    const std::string tree_path = argv[2];
    const std::string oracle_path = argv[3];
    const std::string module_path = argv[4];
    const std::string module_sha = argv[5];
    const std::string capture_path = argv[6];
    const std::string identity_path = argv[7];
    const std::string device = (argc >= 9) ? argv[8] : "pci:0000:00:00.0";
    const bool real_qwen = (argc >= 14);

    Lease lease;
    std::unique_ptr<opoi::VulkanInferenceProcess> infer_proc;
    if (real_qwen) {
      lease.fd = open(argv[13], O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
      require(lease.fd >= 0 && !flock(lease.fd, LOCK_EX | LOCK_NB),
              "Exclusive Vulkan device lease unavailable for real Qwen inference");
      infer_proc = std::make_unique<opoi::VulkanInferenceProcess>(
          argv[9], argv[10], argv[11], argv[12], model_path, lease.fd);
    }

    std::ifstream identity_file(identity_path);
    std::string pom_payout, identity;
    std::getline(identity_file, pom_payout);
    std::getline(identity_file, identity);
    require(bool(identity_file) && !pom_payout.empty() && !identity.empty(), "PoM identity required");

    protowire::KaspadResponse captured;
    for (const auto &r : read_capture(capture_path)) {
      if (r.network == "mainnet") {
        protowire::KaspadResponse reply;
        if (reply.ParseFromArray(r.response_wire.data(), r.response_wire.size()) &&
            reply.has_getblocktemplateresponse() &&
            reply.getblocktemplateresponse().issynced()) {
          captured = std::move(reply);
          break;
        }
      }
    }
    require(captured.has_getblocktemplateresponse(), "Authentic template capture required");
    const auto block = captured.getblocktemplateresponse().block();
    const auto header = header_from_rpc(block.header());
    const auto pph = pre_pow_hash(header);
    const auto extra = opoi::fresh_coinbase_extra_data(identity);

    TemplateFields fields;
    fields.network = "mainnet";
    fields.node_synced = true;
    fields.pay_address = pom_payout;
    fields.extra_data = extra;
    fields.timestamp = header.timestamp;
    fields.daa_score = header.daa_score;
    fields.compact_target_bits = header.bits;
    auto raw = block.SerializeAsString();
    auto wire = captured.SerializeAsString();
    fields.block_wire.assign(raw.begin(), raw.end());
    fields.response_wire.assign(wire.begin(), wire.end());
    fields.consensus_identity_wire = fields.block_wire;

    LiveJobQueue jobs;
    jobs.publish(fields);
    auto job = *jobs.wait_pop();

    GpuWorkCoordinator owner(jobs, 50000, PrimaryVulkanWorkload::batch_limit, secondary_vulkan_batch_size);
    GpuPreemptionCoordinator ownership;
    model::ReadOnlyGguf model(model_path);
    model::SparseMerkleIndex merkle(model, tree_path);
    RustProofVerifier verifier(oracle_path);
    AmdMemoryHandoff handoff;

    const int baseline_threads = current_thread_count();
    PrimaryVulkanWorkload primary(device, module_path, module_sha, owner, model, merkle, verifier, pom_payout, identity);

    int active_threads_observed = 0;
    int released_threads_cycle0 = 0;
    std::size_t total_ai_bytes = 0;
    for (int cycle = 0; cycle < 3; ++cycle) {
      if (device == "pci:0000:00:00.0") handoff.wait_released();
      require(ownership.run_pom([&] {
        require(primary.prepare(), "Resident Vulkan model upload failed");
      }), "PoM admission failed");
      require(primary.allocated(), "Primary workload must report allocated after prepare");

      const int pre_pool_threads = current_thread_count();

      // Step 1: Startup parity batch (1 nonce) -> triggers 4x seed + 8x final_state AVX2 self-test + Rust proof
      {
        auto ticket1 = *owner.reserve(MiningLane::Nvidia, job, pph, 1);
        require(ticket1.count() == 1, "First reservation after prepare must be 1 for startup parity");
        std::unique_ptr<AmdVerifiedCandidate> cand1;
        require(ownership.run_pom([&] {
          cand1 = primary.execute(std::move(ticket1));
        }), "Startup parity execute failed");
        require(cand1 != nullptr && !cand1->rust_summary().empty(), "Startup Rust parity check failed");
      }

      // Step 2: Full batch (1024 nonces) -> initializes ScreenPool(3)
      {
        require(PrimaryVulkanWorkload::batch_limit == 1024, "Expected batch_limit=1024");
        auto ticket_full = *owner.reserve(MiningLane::Nvidia, job, pph, PrimaryVulkanWorkload::batch_limit);
        require(ticket_full.count() == 1024, "Full ticket count must be 1024");
        std::unique_ptr<AmdVerifiedCandidate> cand_full;
        require(ownership.run_pom([&] {
          cand_full = primary.execute(std::move(ticket_full));
        }), "Full 1024 batch execute failed");
        require(cand_full != nullptr, "Completed 1024-nonce batch must return screening candidate");
      }

      const int active_threads = current_thread_count();
      active_threads_observed = active_threads;
      require(active_threads == pre_pool_threads + 3,
              "ScreenPool(3) must add exactly 3 worker threads after 1024-nonce batch");

      // Step 3: Real in-flight AI preemption of a 1024-nonce batch + release()
      std::atomic_bool enqueued{false};
      std::exception_ptr gpu_error;
      std::jthread gpu_thread([&] {
        try {
          auto ticket_preempt = *owner.reserve(MiningLane::Nvidia, job, pph, PrimaryVulkanWorkload::batch_limit);
          require(ticket_preempt.count() == 1024, "Preempted ticket count must be 1024");
          ownership.run_pom([&] {
            auto cancelled = [&] { return ownership.opoi_pending(); };
            auto res = primary.execute(std::move(ticket_preempt), cancelled, [&] {
              enqueued.store(true, std::memory_order_release);
              const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
              while (!ownership.opoi_pending()) {
                if (std::chrono::steady_clock::now() > deadline)
                  throw std::runtime_error("AI preemption timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
              }
            });
            require(!res, "Cancelled in-flight 1024-nonce batch must return nullptr");
          });
        } catch (...) {
          gpu_error = std::current_exception();
          enqueued.store(true, std::memory_order_release);
        }
      });

      const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!enqueued.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      require(enqueued.load(), "1024-nonce batch did not reach after_enqueue hook");

      const auto opoi_res = ownership.run_opoi(
          [&] {
            gpu_thread.join();
            if (gpu_error) std::rethrow_exception(gpu_error);
            owner.pause_for_ai();
            require(owner.in_flight() == 0, "In-flight ticket leaked before AI release");
            primary.release();
            require(!primary.allocated(), "Primary workload still allocated after release()");
            if (device == "pci:0000:00:00.0") handoff.wait_released();
          },
          [&] {
            const int threads_during_ai = current_thread_count();
            if (cycle == 0) released_threads_cycle0 = threads_during_ai;
            require(threads_during_ai <= active_threads - 3,
                    "Workspace ThreadPool threads leaked after primary.release()!");
            require(threads_during_ai == released_threads_cycle0,
                    "Threads accumulated across preemption cycles!");
            if (infer_proc) {
              const auto ans = infer_proc->infer(opoi::official_qwen35_prompt("0011aaff"), 16);
              if (device == "pci:0000:00:00.0") handoff.wait_released();
              const bool has_visible = std::any_of(ans.begin(), ans.end(), [](unsigned char c) { return c > 0x20U; });
              require(has_visible, "Real Vulkan Qwen inference returned empty/blank answer");
              total_ai_bytes += ans.size();
              return ans;
            }
            return std::string("SIMULATED_AI_OK");
          },
          [&] {
            require(owner.resume_after_ai(), "Coordinator did not resume after AI");
          });
      require(!opoi_res.empty(), "OPOI preemption callback failed");
      require(current_thread_count() == released_threads_cycle0, "Threads count mismatch after cycle");
    }

    std::cout << "PIPELINE_LIFECYCLE_AND_PREEMPTION=PASS cycles=3 batch_limit="
              << PrimaryVulkanWorkload::batch_limit
              << " secondary_batch_size=" << secondary_vulkan_batch_size
              << " avx2_self_test=PASS ai_mode=" << (real_qwen ? "REAL_VULKAN_QWEN" : "SIMULATED_CALLBACK")
              << " ai_total_bytes=" << total_ai_bytes
              << " threads_baseline=" << baseline_threads
              << " threads_active=" << active_threads_observed
              << " threads_after_release=" << current_thread_count() << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
