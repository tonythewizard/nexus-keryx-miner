#include "keryx/nexus/developer_config.hpp"
// Real-template capture/read + native AMD candidate + loopback-only submission.
#include "keryx/nexus/amd_submission.hpp"
#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/grpc_runtime.hpp"
#include "keryx/nexus/opoi.hpp"
#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/secondary_work_lane.hpp"
#include "messages.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <fstream>
#include <iostream>
#include <atomic>
using namespace keryx::nexus;using namespace consensus;namespace fs=std::filesystem;
static void expect(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static void save(const fs::path& p,std::span<const std::uint8_t> bytes){if(fs::exists(p))throw std::runtime_error("Existing candidate artifact");std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());if(!f)throw std::runtime_error("Candidate artifact write failed");}
class Mock final:public protowire::RPC::Service {
public:std::atomic_uint sent{0};std::string expected;
 grpc::Status MessageStream(grpc::ServerContext*,grpc::ServerReaderWriter<protowire::KaspadResponse,protowire::KaspadRequest>*stream) override {
  protowire::KaspadRequest request;
  while(stream->Read(&request))if(request.has_submitblockrequest()){
   if(request.submitblockrequest().block().SerializeAsString()!=expected)return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,"AMD assembled block mismatch");
   ++sent;protowire::KaspadResponse response;response.set_id(request.id());response.mutable_submitblockresponse()->set_rejectreason(protowire::SubmitBlockResponseMessage::NONE);stream->Write(response);return grpc::Status::OK;
  }return grpc::Status::OK;
 }
};
int main(int argc,char**argv){try{
 network::GrpcRuntime grpc_runtime;
 if(argc!=9)throw std::runtime_error("Usage: candidate-probe CONFIG MODEL TREE META SPV SHA ORACLE OUTDIR");
 std::ifstream cfg(argv[1]);std::string payout,identity;std::getline(cfg,payout);std::getline(cfg,identity);expect(bool(cfg)&&!payout.empty()&&!identity.empty(),"Missing configured payout/identity");
 const fs::path out=argv[8];expect(!fs::exists(out),"Evidence directory must be new");fs::create_directories(out);
 const auto extra=opoi::fresh_coinbase_extra_data(identity);
 protowire::KaspadResponse response;
 if(const char *capture=std::getenv("NEXUS_TEST_CAPTURE")) {
  bool found=false;for(const auto &record:read_capture(capture)){protowire::KaspadResponse reply;
   if(record.network=="mainnet"&&reply.ParseFromArray(record.response_wire.data(),record.response_wire.size())&&reply.has_getblocktemplateresponse()&&reply.getblocktemplateresponse().issynced()){response=reply;found=true;break;}}
  expect(found,"Offline capture has no synced template");
 } else {
  auto channel=grpc::CreateChannel("127.0.0.1:22110",grpc::InsecureChannelCredentials());auto stub=protowire::RPC::NewStub(channel);
  grpc::ClientContext context;context.set_deadline(std::chrono::system_clock::now()+std::chrono::seconds(10));
  auto stream=stub->MessageStream(&context);protowire::KaspadRequest network,templ;
  network.set_id(101);network.mutable_getcurrentnetworkrequest();templ.set_id(102);
  templ.mutable_getblocktemplaterequest()->set_payaddress(payout);templ.mutable_getblocktemplaterequest()->set_extradata(extra);
  expect(stream->Write(network)&&stream->Write(templ),"Read-only capture request failed");
  bool mainnet=false,found=false;protowire::KaspadResponse reply;
  while(stream->Read(&reply)){
   if(reply.has_getcurrentnetworkresponse())mainnet=reply.getcurrentnetworkresponse().currentnetwork()=="mainnet";
   if(reply.has_getblocktemplateresponse()){response=reply;found=true;}
   if(mainnet&&found)break;
  }
  context.TryCancel();(void)stream->WritesDone();(void)stream->Finish();
  expect(mainnet&&found&&response.getblocktemplateresponse().issynced(),"No synced mainnet RPC template captured");
  const auto wire=response.SerializeAsString();CaptureWriter writer((out/"real.capture").string());writer.append("mainnet",std::vector<std::uint8_t>(wire.begin(),wire.end()));
 }
 const auto original=response;
 model::ReadOnlyGguf model(argv[2]);model::SparseMerkleIndex merkle(model,argv[3]);
 const auto anchor=official_tier_anchor(response.getblocktemplateresponse().block().header().daascore(),response.getblocktemplateresponse().block().header().pomtier());
 const Hash model_id{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
 model.verify_sidecar(argv[3],argv[4],model_id,anchor.root,anchor.chunks);
 const bool resident=std::getenv("NEXUS_TEST_AMD_RESIDENT")&&std::string(std::getenv("NEXUS_TEST_AMD_RESIDENT"))=="1";
 const char* test_uuid=std::getenv("NEXUS_VULKAN_TEST_UUID");
 const std::string test_device=test_uuid?test_uuid:"" NEXUS_SECONDARY_GPU_ID "";
 VulkanWalkEngine engine(test_device,argv[5],argv[6],256,resident?model.chunk_count()/32:0);
 if(resident)engine.load_resident([&](auto tile,auto bytes){model.read_tiles_into(tile,bytes);});RustProofVerifier verifier(argv[7]);
 Mock mock;grpc::ServerBuilder builder;int port=0;builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);builder.RegisterService(&mock);auto server=builder.BuildAndStart();expect(server&&port,"Mock server failed");
 network::SubmitBlockClient client("grpc://127.0.0.1:"+std::to_string(port));
 for(unsigned scenario=0;scenario<7;++scenario){
  auto envelope=original;
  // Clone only: broad target belongs exclusively to a loopback fixture.
  if(scenario)envelope.mutable_getblocktemplateresponse()->mutable_block()->mutable_header()->set_bits(0x2100ffffU);
  if(scenario==5)envelope.mutable_getblocktemplateresponse()->set_issynced(false);
  auto block=envelope.getblocktemplateresponse().block();const auto header=header_from_rpc(block.header());const auto pph=pre_pow_hash(header);
  TemplateFields f;f.network="mainnet";f.node_synced=true;f.pay_address=payout;f.extra_data=extra;f.daa_score=header.daa_score;f.timestamp=header.timestamp;f.compact_target_bits=header.bits;
  auto wire=envelope.SerializeAsString(),raw=block.SerializeAsString();f.response_wire.assign(wire.begin(),wire.end());f.block_wire.assign(raw.begin(),raw.end());f.consensus_identity_wire=f.block_wire;
  if(scenario==6){f.block_wire.back()^=1;f.consensus_identity_wire=f.block_wire;}
  LiveJobQueue queue;queue.publish(f);const auto job=*queue.wait_pop();GpuWorkCoordinator owner(queue,0x4b455259584d3231ULL+scenario);
  auto ticket=*owner.reserve(MiningLane::Amd,job,pph,1);
  AmdCandidatePipeline pipeline(owner,engine,model,merkle,verifier,payout,identity);
  if(scenario>=5){const auto before=engine.stats().submitted;bool rejected=false;
   try{pipeline.execute(ticket);}catch(const std::runtime_error &e){const std::string message=e.what();rejected=scenario==5?message.find("unsynced")!=std::string::npos:message.find("RPC/block snapshot mismatch")!=std::string::npos;}
   expect(rejected&&owner.in_flight()==0&&engine.stats().submitted==before,"Malformed/unsynced RPC reached GPU or leaked nonce");continue;}
  const auto candidate_start=std::chrono::steady_clock::now();
  auto candidate=pipeline.execute(ticket,{},scenario==3?std::function<void()>([&]{queue.invalidate_for_virtual_chain_change();}):scenario==4?std::function<void()>([&]{owner.pause_for_ai();}):std::function<void()>{});
  std::cout<<"AMD_CANDIDATE_TIMING scenario="<<scenario<<" elapsed_ms="
    <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-candidate_start).count()
    <<" allocated_bytes="<<engine.stats().allocated_bytes<<"\n";
  if(scenario==1){bool cancelled=false;
    try{submit_amd_candidate(client,*candidate,true,[]{return true;});}
    catch(const AmdAdmissionChanged &){cancelled=true;}
    expect(cancelled&&mock.sent==0,"Pre-transport cancellation was not explicitly classified");}
  if(scenario>=3){expect(!candidate&&owner.in_flight()==0,"Cancelled candidate reached verification output");
   expect(pipeline.startup_parity_verified()==(scenario==3),"Stale attestation failed or AI pause bypassed");
   continue;}
  expect(bool(candidate)&&owner.in_flight()==1,"Verified candidate lost nonce ownership");
  if(candidate->target_met())save(out/("candidate-"+std::to_string(scenario)+".proof"),candidate->proof());
  if(scenario==0){bool blocked=false;try{submit_amd_candidate(client,*candidate,false);}catch(const std::invalid_argument&){blocked=true;}expect(blocked&&mock.sent==0,"Disabled policy reached transport");std::cout<<"REAL_TEMPLATE_AMD=PASS target_satisfied="<<candidate->target_met()<<" submission=NOT_ATTEMPTED\n";
   candidate.reset();expect(owner.in_flight()==0,"Result destruction leaked nonce reservation");
   for(unsigned bad=0;bad<2;++bad){auto next=*owner.reserve(MiningLane::Amd,job,pph,1);
    AmdCandidatePipeline wrong(owner,engine,model,merkle,verifier,bad?payout:"wrong-payout",bad?identity+"wrong":identity);
    bool rejected=false;try{wrong.execute(next);}catch(const std::runtime_error &e){rejected=std::string(e.what()).find("payout/delegation")!=std::string::npos;}
    expect(rejected&&owner.in_flight()==0,"Wrong payout/identity admitted or leaked reservation");}
   continue;}
  expect(candidate->target_met(),"Loopback-only broad target unexpectedly missed");
  if(scenario==2){queue.invalidate_for_virtual_chain_change();bool blocked=false;try{submit_amd_candidate(client,*candidate,true);}catch(const std::invalid_argument&){blocked=true;}expect(blocked&&mock.sent==1,"Stale verified candidate submitted");continue;}
  network::VerifiedSubmission v;v.nonce=candidate->nonce();network::PomWinner w{v,header.daa_score,candidate->final_state(),header.pom_tier,std::string(reinterpret_cast<const char*>(candidate->proof().data()),candidate->proof().size())};
  mock.expected=network::assemble_pom_winner(block,w,payout,extra).SerializeAsString();
  const auto submitted=submit_amd_candidate(client,*candidate,true);expect(submitted.state==network::SubmissionState::RpcAcknowledged&&mock.sent==1,"AMD mock submission not acknowledged");
  bool duplicate=false;try{submit_amd_candidate(client,*candidate,true);}catch(const std::invalid_argument&){duplicate=true;}expect(duplicate&&mock.sent==1,"Duplicate AMD submission reached mock");
 }
 // Actual Vulkan dispatch cancellation and resume through the same threaded
 // lifecycle used by the combined worker. The NVIDIA slot is CPU-only here.
 {
  const auto &block=original.getblocktemplateresponse().block();const auto header=header_from_rpc(block.header());
  const auto pph=pre_pow_hash(header);TemplateFields f;f.network="mainnet";f.node_synced=true;
  f.pay_address=payout;f.extra_data=extra;f.timestamp=header.timestamp;f.daa_score=header.daa_score;f.compact_target_bits=header.bits;
  const auto wire=original.SerializeAsString(),raw=block.SerializeAsString();f.response_wire.assign(wire.begin(),wire.end());
  f.block_wire.assign(raw.begin(),raw.end());f.consensus_identity_wire=f.block_wire;
  LiveJobQueue queue;queue.publish(f);auto job=*queue.wait_pop();GpuWorkCoordinator owner(queue,0x4b45525958563231ULL,16,1);
  auto primary=*owner.reserve(MiningLane::Nvidia,job,pph,16);
  AmdCandidatePipeline pipeline(owner,engine,model,merkle,verifier,payout,identity);
  std::atomic_uint started{},committed{};std::atomic_bool enqueued{};
  const auto wait=[](const auto &predicate){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!predicate()){if(std::chrono::steady_clock::now()>end)throw std::runtime_error("AMD lifecycle fixture timed out");std::this_thread::sleep_for(std::chrono::milliseconds(1));}};
  SecondaryWorkLane lane(owner,[&](auto ticket,const auto &cancelled,const auto &commit){
    const auto sequence=++started;
    if(sequence>2){wait(cancelled);return false;}
    auto result=pipeline.execute(std::move(ticket),cancelled,sequence==1?std::function<void()>([&]{enqueued=true;wait(cancelled);}):std::function<void()>{});
    if(!result)return false;
    expect(result->nonce()>=primary.first_nonce()+primary.count(),"Threaded AMD nonce overlaps primary range");
    return commit([&]{save(out/"candidate-lifecycle.proof",result->proof());++committed;});
  });
  lane.publish(job,pph);wait([&]{return enqueued.load();});
  lane.pause_and_drain();expect(owner.in_flight()==1&&owner.check(primary)==WorkGate::Ready,"AMD drain touched or waited for primary slot");
  lane.pause_and_drain();queue.invalidate_for_virtual_chain_change();queue.publish(f);job=*queue.wait_pop();lane.publish(job,pph);
  lane.resume();expect(lane.stats().paused,"Nested AI pause resumed GPU early");lane.resume();
  wait([&]{return committed.load()==1;});lane.request_stop();lane.join();lane.rethrow_if_failed();
  expect(lane.stats().verified==1&&lane.stats().discarded>=1&&owner.in_flight()==1,"Threaded lifecycle counters/drain mismatch");
  owner.finish(primary);expect(owner.in_flight()==0&&mock.sent==1,"Lifecycle leaked nonce or submitted a block");
  std::cout<<"AMD_THREADED_LIFECYCLE=PASS actual_gpu_cancel=1 rust_resume=1 nested_ai=PASS primary_slot_retained=true mainnet_submissions=0\n";
 }
 {
  auto envelope=original;envelope.mutable_getblocktemplateresponse()->mutable_block()->mutable_header()->set_bits(0x207fffffU);
  const auto &block=envelope.getblocktemplateresponse().block();const auto header=header_from_rpc(block.header());const auto pph=pre_pow_hash(header);
  const auto target=decode_compact_target(header.bits);
  std::uint64_t start=40000;unsigned expected=0;
  for(unsigned trial=0;trial<16&&!expected;++trial,++start){
    for(unsigned i=0;i<4;++i){const auto seed=block_seed(pph,header.timestamp,start+i,header.daa_score);
      const auto input=gather_v4_model_tiles(seed,model.chunk_count()/32,[&](auto off){std::array<std::uint8_t,1024> tile{};model.read_tile_into(off,tile);return tile;});
      const auto walk=walk_v4_from_tile_sequence(seed,model.chunk_count()/32,input.tiles);
      const bool win=little_endian_leq(pom_pow_value_h3(final_state(walk),pph),target.little_endian);
      if(i==0&&win)break;
      if(i>0&&win){expected=i;break;}
    }
  }
  --start;expect(expected>0,"Could not construct bounded non-first winner fixture");
  TemplateFields f;f.network="mainnet";f.node_synced=true;f.pay_address=payout;f.extra_data=extra;f.daa_score=header.daa_score;f.timestamp=header.timestamp;f.compact_target_bits=header.bits;
  const auto wire=envelope.SerializeAsString(),raw=block.SerializeAsString();f.response_wire.assign(wire.begin(),wire.end());f.block_wire.assign(raw.begin(),raw.end());f.consensus_identity_wire=f.block_wire;
  LiveJobQueue queue;queue.publish(f);auto job=*queue.wait_pop();GpuWorkCoordinator owner(queue,start);
  AmdCandidatePipeline pipeline(owner,engine,model,merkle,verifier,payout,identity);
  auto result=pipeline.execute(*owner.reserve(MiningLane::Amd,job,pph,4));
  expect(result&&result->target_met()&&result->nonce()==start+expected&&!result->proof().empty(),"Non-first winning nonce was rebound or screened incorrectly");
  expect(result->rust_summary().find("target_satisfied=true container=PASS")!=std::string::npos,"Non-first winner was not officially verified");
  std::cout<<"AMD_BATCH_WINNER=PASS batch=4 index="<<expected<<" Rust=PASS target=LOOPBACK_FIXTURE_ONLY submission=DISABLED\n";
 }
 server->Shutdown();server->Wait();expect(mock.sent==1,"Unexpected mock request count");
 if(resident){
  const auto header=header_from_rpc(original.getblocktemplateresponse().block().header());
  const auto pph=pre_pow_hash(header);std::array<std::uint64_t,4> seeds{};
  for(unsigned i=0;i<4;++i)seeds[i]=block_seed(pph,header.timestamp,50000+i,header.daa_score);
  const auto allocated_before=engine.stats().allocated_bytes;
  VulkanWalkBatch many;expect(engine.run(model.chunk_count()/32,seeds,{},{},true,many),"Expanded diagnostics failed");
  for(unsigned i=0;i<4;++i){VulkanWalkBatch single;
    expect(engine.run(model.chunk_count()/32,std::span<const std::uint64_t>(seeds.data()+i,1),{},{},true,single),"Shrunk diagnostics failed");
    expect(std::equal(single.final_states.begin(),single.final_states.end(),many.final_states.begin()+i*1024)&&
      std::equal(single.offsets.begin(),single.offsets.end(),many.offsets.begin()+i*256)&&
      std::equal(single.diagnostic_states.begin(),single.diagnostic_states.end(),many.diagnostic_states.begin()+i*257*1024),
      "Grown/reused trace buffer changed witness bytes");
  }
  const auto allocated_after=engine.stats().allocated_bytes;
  expect(allocated_after>=allocated_before&&allocated_after-allocated_before<4*1024*1024,"Unbounded diagnostic growth");
  std::cout<<"AMD_DIAGNOSTIC_GROW_REUSE=PASS capacity=256 active=4_then_1 canaries=PASS allocated_delta="
    <<allocated_after-allocated_before<<"\n";
 }
 std::cout<<"AMD_CANDIDATE_PIPELINE=PASS real_rpc_template=1 Rust=PASS loopback_submit_ack=1 duplicate=BLOCKED stale=BLOCKED reorg=DISCARDED ai=DISCARDED payout_identity=CHECKED malformed_rpc=REJECTED mainnet_submissions=0\n";
}catch(const std::exception&e){std::cerr<<"AMD_CANDIDATE_PIPELINE=FAIL "<<e.what()<<'\n';return 1;}}
