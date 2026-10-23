#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/consensus/cuda_engine.hpp"
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/grpc_runtime.hpp"
#include "keryx/nexus/gpu_preemption.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/live_job_queue.hpp"
#include "keryx/nexus/opoi.hpp"
#include "messages.grpc.pb.h"
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <vector>

extern char **environ;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace keryx::nexus;
using namespace keryx::nexus::consensus;

namespace {
constexpr char uuid[] = "GPU-00000000-0000-0000-0000-000000000000";
constexpr char challenge_wire[] = "bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710:0011aaff";
constexpr Hash model_id{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,
                        0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
constexpr Hash root{0x2c,0x49,0x71,0x64,0xea,0xf2,0x00,0x78,0xad,0xd2,0x0e,0x82,0xae,0x4e,0x1b,0x0f,
                    0xdb,0x27,0xd3,0xfd,0xd5,0xea,0xef,0xc1,0xc4,0x8f,0x20,0x41,0x11,0xe1,0x4e,0x88};

class ChallengeIngressNode final : public protowire::RPC::Service {
 public:
  std::string inference_result;
  std::atomic_bool challenge_sent{false};
  void release_after_publication() {
    std::lock_guard lock(mutex_);
    release_stream_=true;
    changed_.notify_all();
  }
  grpc::Status MessageStream(grpc::ServerContext *,
      grpc::ServerReaderWriter<protowire::KaspadResponse, protowire::KaspadRequest> *stream) override {
    protowire::KaspadRequest request;
    while (stream->Read(&request)) {
      protowire::KaspadResponse response;
      response.set_id(request.id());
      switch (request.payload_case()) {
        case protowire::KaspadRequest::kGetCurrentNetworkRequest:
          response.mutable_getcurrentnetworkresponse()->set_currentnetwork("Mainnet"); break;
        case protowire::KaspadRequest::kGetInfoRequest:
          response.mutable_getinforesponse()->set_serverversion("opoi-cuda-ingress-fixture");
          response.mutable_getinforesponse()->set_issynced(true); break;
        case protowire::KaspadRequest::kNotifyNewBlockTemplateRequest:
          response.mutable_notifynewblocktemplateresponse(); break;
        case protowire::KaspadRequest::kNotifyVirtualChainChangedRequest:
          response.mutable_notifyvirtualchainchangedresponse(); break;
        case protowire::KaspadRequest::kGetBlockTemplateRequest: {
          const auto &request_template = request.getblocktemplaterequest();
          auto *templ = response.mutable_getblocktemplateresponse();
          templ->set_issynced(true);
          auto *header = templ->mutable_block()->mutable_header();
          header->set_daascore(899);
          header->set_bits(0x207fffffU);
          header->set_timestamp(1700000999);
          if (request_template.inferenceresult().empty() && !challenge_sent.exchange(true)) {
            templ->set_inferencechallenge(challenge_wire);
          } else if (!request_template.inferenceresult().empty()) {
            inference_result = request_template.inferenceresult();
            const std::string prefix = std::string(challenge_wire) + ":";
            if (inference_result.rfind(prefix, 0) != 0 || inference_result.size() <= prefix.size())
              return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid official OPoI inferenceResult");
            // This is an isolated fixture endpoint: the response is protocol-tested, never sent to keryxd.
            header->set_daascore(901);
          } else {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "expected OPoI response request");
          }
          break;
        }
        default: continue;
      }
      if (!stream->Write(response)) break;
      if(request.payload_case()==protowire::KaspadRequest::kGetBlockTemplateRequest &&
         !request.getblocktemplaterequest().inferenceresult().empty()) {
        std::unique_lock lock(mutex_);
        if(!changed_.wait_for(lock,20s,[&]{return release_stream_;}))
          return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,"test did not observe post-OPoI job publication");
      }
      request.Clear();
    }
    return grpc::Status::OK;
  }
 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool release_stream_{};
};

std::uint32_t read32(const std::vector<std::uint8_t> &v, std::size_t &p) {
  if (p + 4 > v.size()) throw std::runtime_error("truncated state trace u32");
  std::uint32_t x=0; for(unsigned i=0;i<4;++i)x|=static_cast<std::uint32_t>(v[p++])<<(8U*i); return x;
}
std::uint64_t read64(const std::vector<std::uint8_t> &v, std::size_t &p) {
  if (p + 8 > v.size()) throw std::runtime_error("truncated state trace u64");
  std::uint64_t x=0; for(unsigned i=0;i<8;++i)x|=static_cast<std::uint64_t>(v[p++])<<(8U*i); return x;
}
void write32(std::ostream &o,std::uint32_t x){for(unsigned i=0;i<4;++i)o.put(static_cast<char>(x>>(8U*i)));}
void write64(std::ostream &o,std::uint64_t x){for(unsigned i=0;i<8;++i)o.put(static_cast<char>(x>>(8U*i)));}
void verify_rust(const fs::path &oracle,const fs::path &context,const fs::path &trace,const fs::path &proof) {
  pid_t pid{}; auto a=oracle.string(),b=context.string(),c=trace.string(),d=proof.string(); char mode[]="verify-real";
  std::array<char*,6> argv{a.data(),mode,b.data(),c.data(),d.data(),nullptr};
  const int rc=posix_spawn(&pid,oracle.c_str(),nullptr,nullptr,argv.data(),environ);
  if(rc) throw std::runtime_error("could not launch Rust verifier for CUDA preemption trace");
  int status=0; for(;;){const auto rc=waitpid(pid,&status,0);if(rc==pid)break;if(rc<0&&errno==EINTR)continue;throw std::runtime_error("waitpid failed for Rust verifier");}
  if(!WIFEXITED(status)||WEXITSTATUS(status)!=0) throw std::runtime_error("Rust verifier rejected CUDA trace after OPoI preemption");
}
}

int main(int argc,char **argv) {
  network::GrpcRuntime grpc_runtime;
  try {
    if(argc!=10) throw std::runtime_error("usage: nexus-opoi-preemption-cuda-test MODEL TREE METADATA TRACE CONTEXT PROOF ORACLE LLAMA_LIBRARY OUTPUT_TRACE");
    const char *visible=std::getenv("CUDA_VISIBLE_DEVICES");
    if(!visible||std::string(visible)!=uuid) throw std::runtime_error("CUDA_VISIBLE_DEVICES must select only authorized primary NVIDIA GPU UUID");
    const fs::path model_path=argv[1],tree_path=argv[2],metadata_path=argv[3];
    const fs::path source_trace=argv[4],context_path=argv[5],proof_path=argv[6],oracle=argv[7];
    const fs::path llama=argv[8],post_trace=argv[9];
    std::ifstream input(source_trace,std::ios::binary);
    if(!input) throw std::runtime_error("cannot open previously verified real GGUF trace");
    std::vector<std::uint8_t> original((std::istreambuf_iterator<char>(input)),{});
    if(original.size()<28||std::string(reinterpret_cast<const char*>(original.data()),4)!="NXST")
      throw std::runtime_error("invalid verified trace format");
    std::size_t pos=4; const auto seed=read64(original,pos); const auto nonce=read64(original,pos);
    if(read32(original,pos)!=257||read32(original,pos)!=256||original.size()!=28+257*tile_bytes+256*8)
      throw std::runtime_error("verified trace dimensions or length mismatch");
    const auto expected_states=std::vector<std::uint8_t>(original.begin()+28,original.begin()+28+257*tile_bytes);
    std::vector<std::uint64_t> expected_offsets(256);
    pos=28+257*tile_bytes; for(auto &x:expected_offsets)x=read64(original,pos);

    model::ReadOnlyGguf gguf(model_path);
    (void)gguf.verify_sidecar(tree_path,metadata_path,model_id,root,203469888);
    const auto tile_count=gguf.chunk_count()/chunks_per_tile;
    std::vector<std::uint64_t> ids; std::vector<std::uint32_t> slots;
    std::vector<std::uint8_t> packed; std::unordered_map<std::uint64_t,std::uint32_t> map;
    slots.reserve(walk_steps); packed.reserve(walk_steps*tile_bytes);
    for(const auto offset:expected_offsets){
      if(offset>=tile_count) throw std::runtime_error("trace tile offset outside canonical GGUF index");
      const auto [it,inserted]=map.emplace(offset,static_cast<std::uint32_t>(ids.size()));
      if(inserted){ ids.push_back(offset); const auto tile=gguf.read_tile(offset); packed.insert(packed.end(),tile.begin(),tile.end()); }
      slots.push_back(it->second);
    }
    const auto model_identity=hex(model_id.data(),model_id.size())+hex(root.data(),root.size());
    auto engine=std::make_unique<CudaWalkEngine>(uuid,1);
    GpuPreemptionCoordinator coordinator;
    std::mutex mutex; std::condition_variable changed; bool launched=false; bool kernel_in_flight=false;
    ChallengeIngressNode service;
    grpc::ServerBuilder server_builder; int port=0;
    server_builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);
    server_builder.RegisterService(&service);
    auto server=server_builder.BuildAndStart();
    if(!server||port==0) throw std::runtime_error("could not start isolated OPoI gRPC fixture server");
    LiveJobQueue live_queue; std::atomic_bool stop_requested{false}; std::atomic_int client_result{-99};
    std::exception_ptr ingress_error;
    CudaWalkBatch first; std::exception_ptr pom_error;
    std::thread pom([&]{try{
      const bool ran=coordinator.run_pom([&]{
        engine->run(tile_count,model_identity,{seed},packed,ids,slots,true,first,[&](bool active){
          std::unique_lock lock(mutex); kernel_in_flight=active; launched=true; changed.notify_all();
          if(active) {
            const auto deadline=std::chrono::steady_clock::now()+10s;
            lock.unlock();
            while(!coordinator.opoi_pending()&&std::chrono::steady_clock::now()<deadline)
              std::this_thread::sleep_for(1ms);
            if(!coordinator.opoi_pending()) throw std::runtime_error("gRPC fixture challenge did not reach active PoM kernel");
          }
        });
        if(first.offsets!=expected_offsets||first.diagnostic_states!=expected_states)
          throw std::runtime_error("CUDA states/offsets diverged from verified real-model trace");
      });
      if(!ran) throw std::runtime_error("first CUDA PoM batch was not admitted");
    }catch(...){pom_error=std::current_exception();}});
    {
      std::unique_lock lock(mutex);
      if(!changed.wait_for(lock,10s,[&]{return launched;})) {
        lock.unlock(); pom.join();
        if(pom_error) std::rethrow_exception(pom_error);
        throw std::runtime_error("CUDA kernel launch hook was not reached");
      }
    }
    std::string answer,wire; std::exception_ptr opoi_error;
    std::thread client([&]{try{
      client_result.store(run_live_job_client("grpc://127.0.0.1:"+std::to_string(port),"fixture-payout","",
        live_queue,1,0,stop_requested,[&](const std::string &wire_challenge){
          const auto parsed=opoi::parse_challenge(wire_challenge);
          if(wire_challenge!=challenge_wire) throw std::runtime_error("gRPC OPoI challenge fixture bytes changed");
          wire=coordinator.run_opoi(
            [&]{engine.reset(); std::cout<<"OPOI_PREEMPT=KERNEL_DRAINED source=grpc_ingress pom_buffers_released=true\n"<<std::flush;},
            [&]{opoi::StockLlamaRuntime runtime(llama.string(),model_path.string()); answer=runtime.answer(parsed); return opoi::format_inference_result(parsed,answer);},
            [&]{engine=std::make_unique<CudaWalkEngine>(uuid,1);});
          return wire;
        }),std::memory_order_release);
    }catch(...){ingress_error=std::current_exception();}});
    pom.join();
    if(pom_error) std::rethrow_exception(pom_error);
    if(ingress_error) std::rethrow_exception(ingress_error);
    const auto publication_deadline=std::chrono::steady_clock::now()+90s;
    while(live_queue.published_count()==0 && client_result.load(std::memory_order_acquire)==-99 &&
          std::chrono::steady_clock::now()<publication_deadline) std::this_thread::sleep_for(10ms);
    if(live_queue.published_count()==0) {
      service.release_after_publication();
      client.join(); server->Shutdown();
      throw std::runtime_error("gRPC client did not publish a fresh post-OPoI job");
    }
    auto fresh_job=live_queue.wait_pop();
    if(!fresh_job||fresh_job->generation!=1||!fresh_job->lease.valid()||fresh_job->fields.daa_score!=901)
      throw std::runtime_error("post-OPoI gRPC template was not published as a fresh live job");
    service.release_after_publication();
    client.join();
    server->Shutdown();
    if(client_result.load(std::memory_order_acquire)!=0) throw std::runtime_error("gRPC live OPoI handler did not exit cleanly");
    if(fresh_job->lease.valid()) throw std::runtime_error("client stream close failed to invalidate the post-OPoI job lease");
    if(!kernel_in_flight) throw std::runtime_error("fixture challenge did not overlap an incomplete CUDA kernel event");
    if(answer.empty()||wire.rfind(std::string(challenge_wire)+":",0)!=0||service.inference_result!=wire)
      throw std::runtime_error("stock OPoI inference output/wire was invalid");

    fs::create_directories(post_trace.parent_path());
    std::ofstream out(post_trace,std::ios::binary|std::ios::trunc);
    out.write("NXST",4);write64(out,seed);write64(out,nonce);write32(out,257);write32(out,256);
    out.write(reinterpret_cast<const char*>(first.diagnostic_states.data()),static_cast<std::streamsize>(first.diagnostic_states.size()));
    for(auto x:first.offsets)write64(out,x);out.close();
    verify_rust(oracle,context_path,post_trace,proof_path);

    CudaWalkBatch resumed;
    const bool resumed_pom=coordinator.run_pom([&]{engine->run(tile_count,model_identity,{seed},packed,ids,slots,true,resumed);});
    if(!resumed_pom||resumed.offsets!=expected_offsets||resumed.diagnostic_states!=expected_states)
      throw std::runtime_error("PoM did not resume with the same authentic walk after OPoI");
    coordinator.request_stop(); engine.reset();
    std::error_code ignored; fs::remove(post_trace,ignored);
    std::cout<<"OPOI_CUDA_PREEMPTION=PASS uuid="<<uuid<<" same_process=true kernel_in_flight_at_grpc_challenge=true "
             <<"grpc_inferenceResult=PASS fresh_job_generation="<<fresh_job->generation
             <<" rust_trace=PASS answer_bytes="<<answer.size()<<" resume_kernel=PASS submit=DISABLED\n";
    return 0;
  }catch(const std::exception &e){std::cerr<<"OPOI_CUDA_PREEMPTION=FAIL detail="<<e.what()<<" submit=DISABLED\n";return 1;}
}
