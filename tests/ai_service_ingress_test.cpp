#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/grpc_runtime.hpp"
#include "keryx/nexus/live_job_queue.hpp"
#include "messages.pb.h"

#include <atomic>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
  keryx::nexus::network::GrpcRuntime grpc_runtime;
  if (argc != 2) throw std::runtime_error("mock endpoint required");
  std::atomic_bool stop{false};
  keryx::nexus::LiveJobQueue jobs;
  unsigned requests = 0;
  bool was_backfill = false;
  bool backfill_complete = false;
  const int rc = keryx::nexus::run_live_job_client(
      argv[1], "keryx:qrxpcusyrxjxghfdumcxm2rqw4dhe3n9hyqpvgn2wfyldltf99w2xhnajuhte",
      "offline-ai-ingress-test", jobs, 0, 0, stop, {},
      [&](const protowire::RpcBlock &block, bool backfill) {
        if (!block.has_header()) throw std::runtime_error("backfill block missing header");
        for (const auto &tx : block.transactions()) {
          std::string why;
          const auto request = keryx::nexus::ai::parse_request(tx, block.header().daascore(), &why);
          if (!request) continue;
          if (request->model_id[0] != 0xbd || request->max_tokens != 64)
            throw std::runtime_error("mock AiRequest fields differ from official fixture");
          ++requests;
          was_backfill = backfill;
          stop.store(true, std::memory_order_relaxed);
        }
      }, {}, [&](bool complete, std::size_t hashes, std::size_t unavailable) {
        if (!complete || hashes != 1 || unavailable != 0)
          throw std::runtime_error("startup backfill completion reported unexpected coverage");
        backfill_complete = true;
      });
  if (rc != 0 || requests != 1 || !was_backfill || !backfill_complete)
    throw std::runtime_error("BlockAdded startup backfill did not discover exactly one official AiRequest");
  std::cout << "AI_REQUEST_STARTUP_BACKFILL=PASS requests=1 assigned=false submit=DISABLED\n";
}
