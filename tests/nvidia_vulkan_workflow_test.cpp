// Real Vulkan-only PoM -> assigned public/private AI -> signed mock response -> PoM.
// No CUDA engine/library is linked into this fixture. All writes are loopback.
#include "keryx/nexus/primary_vulkan_workload.hpp"
#include "keryx/nexus/amd_submission.hpp"
#include "keryx/nexus/ai_response_service.hpp"
#include "keryx/nexus/vulkan_inference_process.hpp"
#include "keryx/nexus/gpu_preemption.hpp"
#include "keryx/nexus/grpc_runtime.hpp"
#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/grpc_header.hpp"
#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/opoi.hpp"
#include "messages.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <blake2.h>
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <sys/file.h>
using namespace keryx::nexus;
using namespace keryx::nexus::ai;
using namespace keryx::nexus::consensus;
namespace fs=std::filesystem;
namespace {
#include "ai_response_fixture_support.hpp"
void no_cuda_runtime() {
  std::ifstream in("/proc/self/maps");std::string line;
  while(std::getline(in,line))if(line.find("libcudart.so")!=line.npos||line.find("libcuda.so")!=line.npos)
    throw std::runtime_error("Native CUDA library mapped in Vulkan-only fixture");
}
struct Lease {
  int fd{-1};
  ~Lease(){if(fd>=0)close(fd);}
};
}
int main(int argc,char **argv){try{
  // PUBLIC PRIVATE OUTPUT MODEL TREE META ORACLE MODULE SHA HELPER HELPER_SHA LIB LIB_SHA CAPTURE IDENTITY
  require(argc==16,"Expected explicit pinned Vulkan fixture artifacts");
  network::GrpcRuntime grpc;
  const fs::path out=argv[3];require(!fs::exists(out),"New private output directory required");
  umask(0077);fs::create_directories(out);
  const auto public_f=fixture(argv[1]),private_f=fixture(argv[2]);
  require(stock_verify_escrow_delegation(payout,public_f.escrow,public_f.cert),"Fixture delegation invalid");
  require(public_f.escrow==private_f.escrow&&public_f.cert==private_f.cert,"Fixture identities differ");
  std::ifstream identity_file(argv[15]);std::string pom_payout,identity;
  std::getline(identity_file,pom_payout);std::getline(identity_file,identity);
  require(bool(identity_file)&&!pom_payout.empty()&&!identity.empty(),"PoM template identity required");
  protowire::KaspadResponse captured;
  for(const auto &r:read_capture(argv[14]))if(r.network=="mainnet") {
    protowire::KaspadResponse reply;
    if(reply.ParseFromArray(r.response_wire.data(),r.response_wire.size())&&reply.has_getblocktemplateresponse()&&reply.getblocktemplateresponse().issynced()) {captured=std::move(reply);break;}
  }
  require(captured.has_getblocktemplateresponse(),"Authentic template capture required");
  // Clone only. Its easy target belongs exclusively to the loopback mock.
  captured.mutable_getblocktemplateresponse()->mutable_block()->mutable_header()->set_bits(0x2100ffffU);
  const auto block=captured.getblocktemplateresponse().block();const auto header=header_from_rpc(block.header());
  const auto pph=pre_pow_hash(header);const auto extra=opoi::fresh_coinbase_extra_data(identity);
  TemplateFields fields;fields.network="mainnet";fields.node_synced=true;fields.pay_address=pom_payout;fields.extra_data=extra;
  fields.timestamp=header.timestamp;fields.daa_score=header.daa_score;fields.compact_target_bits=header.bits;
  auto raw=block.SerializeAsString(),wire=captured.SerializeAsString();fields.block_wire.assign(raw.begin(),raw.end());
  fields.response_wire.assign(wire.begin(),wire.end());fields.consensus_identity_wire=fields.block_wire;
  LiveJobQueue jobs;jobs.publish(fields);auto job=*jobs.wait_pop();
  GpuWorkCoordinator owner(jobs,40000,256,256);GpuPreemptionCoordinator ownership;
  model::ReadOnlyGguf model(argv[4]);model::SparseMerkleIndex merkle(model,argv[5]);
  const Hash model_id{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
  const auto anchor=official_tier_anchor(header.daa_score,header.pom_tier);
  model.verify_sidecar(argv[5],argv[6],model_id,anchor.root,anchor.chunks);
  { // The persistent Rust verifier is an intentional child until this scope ends.
  RustProofVerifier verifier(argv[7]);
  require(opoi::valid_coinbase_extra_data(extra,identity),"PoM fixture identity/fresh-tag binding invalid");
  PrimaryVulkanWorkload primary("GPU-00000000-0000-0000-0000-000000000000",argv[8],argv[9],owner,model,merkle,verifier,pom_payout,identity);
  Lease lease;lease.fd=open("/opt/nexus-data/state/gpu-devices/lease-GPU-00000000-0000-0000-0000-000000000000.lock",O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
  require(lease.fd>=0&&!flock(lease.fd,LOCK_EX|LOCK_NB),"Exclusive NVIDIA Vulkan lease unavailable");
  opoi::VulkanInferenceProcess infer(argv[10],argv[11],argv[12],argv[13],argv[4],lease.fd);
  SubmitMock mock;mock.set_template_capture(argv[14]);grpc::ServerBuilder builder;int port=0;
  builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);builder.RegisterService(&mock);
  auto server=builder.BuildAndStart();require(server&&port,"Loopback mock startup failed");
  const auto endpoint="grpc://127.0.0.1:"+std::to_string(port);network::SubmitBlockClient submit(endpoint);
  unsigned cycle=0;
  for(const auto *request:{&public_f,&private_f}) {
    const bool sealed=cycle!=0;const auto daa=sealed?mainnet_private_inference_gate_daa+100:mainnet_private_inference_gate_daa-1;
    std::atomic_bool enqueued=false;std::exception_ptr gpu_error;
    require(ownership.run_pom([&]{require(primary.prepare(),"Resident Vulkan model upload failed");}),"Initial PoM admission refused");
    std::jthread gpu([&]{try{
      auto ticket=*owner.reserve(MiningLane::Nvidia,job,pph,1);
      ownership.run_pom([&]{
        auto cancelled=[&]{return ownership.opoi_pending();};
        auto result=primary.execute(ticket,cancelled,[&]{
          enqueued=true;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
          while(!ownership.opoi_pending()) {
            if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("AI preemption not announced");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        });
        require(!result,"Cancelled in-flight Vulkan work returned a candidate");
      });
    }catch(...){gpu_error=std::current_exception();enqueued=true;}});
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(!enqueued&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(enqueued,"Vulkan kernel did not enter active enqueue hook");
    AiResponseService service(config_for(*request,out/(sealed?"private.journal":"public.journal")));service.recover();
    discover_via_live_grpc(mock,endpoint,service,request->tx,daa);
    std::string generated_answer;
    auto effects=effects_for(endpoint,[&](std::string_view prompt,std::uint32_t tokens){
      try {
      return ownership.run_opoi([&]{
        gpu.join();if(gpu_error)std::rethrow_exception(gpu_error);
        owner.pause_for_ai();require(owner.in_flight()==0,"In-flight nonce leaked before AI");primary.release();
        require(!primary.allocated(),"PoM device retained before Vulkan inference");
      },[&]{generated_answer=infer.infer(opoi::official_qwen35_ai_prompt(prompt),tokens);return generated_answer;},[&]{
        require(owner.resume_after_ai(),"PoM nonce owner did not resume");
      });
      }catch(const std::exception &e){std::cerr<<"VULKAN_FIXTURE_INFERENCE_ERROR "<<e.what()<<'\n'<<std::flush;throw;}
    },mock);
    require(service.serve(request->tx,daa,providers_for(*request),effects)==ServeResult::Submitted,"Real Vulkan response failed");
    require(mock.count()==int(cycle+1),"Missing or duplicate AI submission");
    auto parsed=parse_request(request->tx,daa,nullptr,"mainnet");require(bool(parsed),"AI fixture parse failed");
    std::vector<std::uint8_t> opened;
    require(stock_verify_response(unhex(mock.last().payload()),parsed->request_hash,request->escrow,
      sealed?std::optional(request->root):std::nullopt,sealed?&opened:nullptr),"Official Rust response signature/sealing verification failed");
    if(sealed)require(std::string(opened.begin(),opened.end())==generated_answer,
      "Private response did not open to the real Vulkan inference output");
    protowire::RpcBlock observed;observed.mutable_header()->set_daascore(daa+1);observed.add_transactions()->CopyFrom(mock.last());
    require(service.observe_block(observed)==1,"AI response reconciliation failed");
    require(service.serve(request->tx,daa,providers_for(*request),effects)==ServeResult::Duplicate,"AI response was not deduplicated");
    require(mock.count()==int(cycle+1),"Duplicate AI response reached mock");
    jobs.invalidate_for_virtual_chain_change();jobs.publish(fields);job=*jobs.wait_pop();
    require(ownership.run_pom([&]{
      require(primary.prepare(),"Vulkan model did not restore after AI");
      auto candidate=primary.execute(*owner.reserve(MiningLane::Nvidia,job,pph,1));
      require(candidate&&candidate->target_met()&&!candidate->rust_summary().empty(),"Resumed Vulkan proof failed Rust");
      network::VerifiedSubmission v;v.nonce=candidate->nonce();
      network::PomWinner winner{v,header.daa_score,candidate->final_state(),header.pom_tier,
        std::string(reinterpret_cast<const char*>(candidate->proof().data()),candidate->proof().size())};
      mock.expect_block(network::assemble_pom_winner(block,winner,pom_payout,extra));
      require(submit_amd_candidate(submit,*candidate,true).state==network::SubmissionState::RpcAcknowledged,"Verified PoM mock ACK missing");
      bool duplicate=false;try{submit_amd_candidate(submit,*candidate,true);}catch(const std::invalid_argument&){duplicate=true;}
      require(duplicate,"Duplicate PoM submission admitted");
    }),"Resumed PoM admission failed");
    require(mock.block_count()==cycle+1,"PoM mock submission count mismatch");
    no_cuda_runtime();++cycle;
    std::cout<<"NVIDIA_VULKAN_WORKFLOW_CYCLE=PASS kind="<<(sealed?"private":"public")
      <<" active_pom_preempted=true signed_response=PASS sealed="<<(sealed?"true":"false")
      <<" reconciled=true resumed_rust_proof=true mock_only=true\n"<<std::flush;
  }
  primary.release();require(owner.in_flight()==0,"Final nonce reservation leak");
  no_cuda_runtime();server->Shutdown();server->Wait();
  }
  int status{};require(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD,"Unreaped child remains after complete teardown");
  std::cout<<"NVIDIA_VULKAN_WORKFLOW=PASS cycles=2 ai_mock_ack=2 pom_mock_ack=2 cuda_runtime=ABSENT mainnet_submissions=0\n";
}catch(const std::exception &e){std::cerr<<"NVIDIA_VULKAN_WORKFLOW=FAIL "<<e.what()<<'\n';return 1;}}
