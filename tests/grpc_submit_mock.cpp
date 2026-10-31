#include "keryx/nexus/capture.hpp"
#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/gguf_reader.hpp"
#include "keryx/nexus/grpc_header.hpp"
#include "keryx/nexus/network/submission_pipeline.hpp"
#include "messages.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;
namespace fs = std::filesystem;
using namespace keryx::nexus;
using namespace keryx::nexus::consensus;

namespace {
constexpr std::uint32_t easy_test_bits=0x2100ffffU;
constexpr Hash official_model{0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
constexpr Hash official_root{0x2c,0x49,0x71,0x64,0xea,0xf2,0x00,0x78,0xad,0xd2,0x0e,0x82,0xae,0x4e,0x1b,0x0f,0xdb,0x27,0xd3,0xfd,0xd5,0xea,0xef,0xc1,0xc4,0x8f,0x20,0x41,0x11,0xe1,0x4e,0x88};
void write(std::ostream&o,const void*p,std::size_t n){o.write(static_cast<const char*>(p),static_cast<std::streamsize>(n));if(!o)throw std::runtime_error("proof fixture write failed");}
void w32(std::ostream&o,std::uint32_t x){std::array<std::uint8_t,4>b{};for(unsigned i=0;i<4;++i)b[i]=static_cast<std::uint8_t>(x>>(8*i));write(o,b.data(),b.size());}
void w64(std::ostream&o,std::uint64_t x){std::array<std::uint8_t,8>b{};for(unsigned i=0;i<8;++i)b[i]=static_cast<std::uint8_t>(x>>(8*i));write(o,b.data(),b.size());}
void whash(std::ostream&o,const Hash&h){write(o,h.data(),h.size());}
void verify_rust(const fs::path&oracle,const fs::path&context,const fs::path&trace,const fs::path&proof){
  pid_t pid{};std::string e=oracle.string(),c=context.string(),t=trace.string(),p=proof.string();char mode[]="verify-real";
  std::array<char*,6>a{e.data(),mode,c.data(),t.data(),p.data(),nullptr};
  if(posix_spawn(&pid,e.c_str(),nullptr,nullptr,a.data(),environ)!=0)throw std::runtime_error("spawn official Rust verifier failed");
  int status{};for(;;){const auto n=waitpid(pid,&status,0);if(n==pid)break;if(n<0&&errno==EINTR)continue;throw std::runtime_error("waitpid failed for official Rust verifier");}
  if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw std::runtime_error("official Rust verifier rejected authentic GGUF proof fixture");
}

struct Fixture { protowire::RpcBlock template_block; network::PomWinner winner; fs::path proof,context,trace; };

Fixture make_fixture(model::ReadOnlyGguf&model,model::SparseMerkleIndex&merkle,
    const protowire::RpcBlock&base,const protowire::KaspadResponse&base_response,
    const fs::path&oracle,const fs::path&out,std::uint64_t nonce,
    const std::string&payout,const std::string&extra){
  protowire::RpcBlock test_template=base;
  test_template.mutable_header()->set_bits(easy_test_bits);
  auto header=header_from_rpc(test_template.header());
  const auto immutable_test_template=test_template;
  const auto target=decode_compact_target(easy_test_bits);
  if(target.zero)throw std::runtime_error("synthetic test target compact bits are invalid");
  const auto pph=pre_pow_hash(header);
  const auto seed=block_seed(pph,header.timestamp,nonce,header.daa_score);
  const auto ntiles=model.chunk_count()/32;
  const auto gathered=gather_v4_model_tiles(seed,ntiles,[&](std::uint64_t offset){
    const auto bytes=model.read_tile(offset);std::array<std::uint8_t,tile_bytes>tile{};std::copy(bytes.begin(),bytes.end(),tile.begin());return tile;
  });
  const auto walk=walk_v4_from_tile_sequence(seed,ntiles,gathered.tiles);
  const auto final=final_state(walk);const auto pow=pom_pow_value_h3(final,pph);
  if(!little_endian_leq(pow,target.little_endian))throw std::runtime_error("test-only easy target unexpectedly missed");
  const auto proof=prove_v4_from_walk(walk,header.pom_tier,gathered.tiles,[&](std::uint64_t offset){return merkle.tile_path(offset);});
  const auto wire=borsh_v4_container(proof,final,pow);
  const std::string proof_wire(reinterpret_cast<const char*>(wire.data()),wire.size());

  protowire::KaspadResponse response=base_response;
  response.mutable_getblocktemplateresponse()->mutable_block()->CopyFrom(test_template);
  const auto response_bytes=response.SerializeAsString();
  const auto response_digest=blake3_digest(reinterpret_cast<const std::uint8_t*>(response_bytes.data()),response_bytes.size());
  const auto suffix=std::to_string(nonce)+"-"+std::to_string(static_cast<unsigned long long>(::getpid()));
  const auto proof_path=out/("proof-"+suffix+".borsh"),context_path=out/("proof-"+suffix+".context"),trace_path=out/("proof-"+suffix+".trace");
  for(const auto&p:{proof_path,context_path,trace_path})if(fs::exists(p))throw std::runtime_error("refusing to overwrite existing proof test artifact");
  {std::ofstream f(proof_path,std::ios::binary);write(f,wire.data(),wire.size());}
  {std::ofstream f(context_path,std::ios::binary);write(f,"NXRF",4);w64(f,seed);w64(f,model.chunk_count());write(f,&header.pom_tier,1);whash(f,official_root);whash(f,target.little_endian);whash(f,pph);w64(f,header.timestamp);w64(f,nonce);w64(f,header.daa_score);w64(f,1);whash(f,response_digest);w64(f,final);whash(f,pow);w32(f,static_cast<std::uint32_t>(wire.size()));write(f,wire.data(),wire.size());}
  {std::ofstream f(trace_path,std::ios::binary);write(f,"NXST",4);w64(f,seed);w64(f,nonce);w32(f,static_cast<std::uint32_t>(walk.states.size()));w32(f,static_cast<std::uint32_t>(walk.offsets.size()));for(const auto&s:walk.states)write(f,s.data(),s.size());for(auto x:walk.offsets)w64(f,x);}
  verify_rust(oracle,context_path,trace_path,proof_path);
  auto *winner_header=test_template.mutable_header();winner_header->set_nonce(nonce);
  if(header.daa_score>=43'450'000)winner_header->set_pomfinalstate(final);
  if(header.daa_score>=h6_activation_daa)winner_header->set_pomtier(header.pom_tier);
  test_template.set_pomproof(proof_wire);
  network::VerifiedSubmission verified;
  verified.job_id=hex(response_digest.data(),response_digest.size());verified.generation=1;verified.nonce=nonce;
  verified.pom_verified=true;verified.model_verified=true;verified.merkle_verified=true;
  verified.header_seed_verified=true;verified.target_satisfied=true;verified.job_fresh=true;
  verified.generation_matches=true;verified.miner_configured=!payout.empty()&&!extra.empty();
  if(!verified.miner_configured)throw std::runtime_error("test payout/escrow configuration is empty");
  return {immutable_test_template,network::PomWinner{std::move(verified),header.daa_score,final,header.pom_tier,proof_wire},proof_path,context_path,trace_path};
}
}

class SubmitMock final:public protowire::RPC::Service{
public:
  std::atomic<unsigned>requests{0};std::map<std::uint64_t,std::string>expected_blocks;
  std::map<std::string,protowire::RpcBlock> blocks;
  std::map<std::string,std::vector<bool>> chain_patterns;
  std::map<std::string,std::size_t> block_reads;
  std::set<std::string> visible_blocks;
  std::mutex state_mutex;
  std::string sink_hash{"mock-sink"};
  std::uint64_t sink_blue_score{};
  grpc::Status MessageStream(grpc::ServerContext* context,grpc::ServerReaderWriter<protowire::KaspadResponse,protowire::KaspadRequest>*stream)override{
    protowire::KaspadRequest request;
    while(stream->Read(&request)){
      if(request.has_getblockrequest()){
        const auto hash=request.getblockrequest().hash();
        protowire::KaspadResponse response;response.set_id(request.id());auto *payload=response.mutable_getblockresponse();
        std::lock_guard state_lock(state_mutex);
        const auto found=blocks.find(hash);
        if(found==blocks.end()||!visible_blocks.contains(hash))payload->mutable_error()->set_message("block not found");
        else {
          payload->mutable_block()->CopyFrom(found->second);
          auto *verbose=payload->mutable_block()->mutable_verbosedata();
          const auto pattern=chain_patterns.find(hash);
          if(pattern!=chain_patterns.end()&&!pattern->second.empty()){
            auto &read=block_reads[hash];
            verbose->set_ischainblock(pattern->second[std::min(read,pattern->second.size()-1)]);
            ++read;
          }
        }
        if(!stream->Write(response))return grpc::Status(grpc::StatusCode::UNAVAILABLE,"mock GetBlock response write failed");
        continue;
      }
      if(request.has_getblockdaginforequest()){
        protowire::KaspadResponse response;response.set_id(request.id());
        response.mutable_getblockdaginforesponse()->set_sink(sink_hash);
        if(!stream->Write(response))return grpc::Status(grpc::StatusCode::UNAVAILABLE,"mock DAG info response write failed");
        continue;
      }
      if(!request.has_submitblockrequest())continue;
      const auto &payload=request.submitblockrequest();const auto &block=payload.block();
      const auto number=requests.fetch_add(1)+1;
      const auto expected=expected_blocks.find(block.header().nonce());
      if(expected==expected_blocks.end()||block.SerializeAsString()!=expected->second||payload.allownondaablocks())
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,"full assembled block mismatch");
      if(number==7) {
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!context->IsCancelled()&&std::chrono::steady_clock::now()<until)
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return grpc::Status(grpc::StatusCode::CANCELLED,"mock AI preemption");
      }
      if(number==4)return grpc::Status(grpc::StatusCode::UNAVAILABLE,"mock dropped response after request");
      if(number==1||number==5||number==6) {
        const auto header=header_from_rpc(block.header());
        const auto digest=block_hash_for_daa(header,block.header().nonce(),static_cast<std::uint64_t>(block.header().timestamp()));
        static constexpr char digits[]="0123456789abcdef";std::string hash(digest.size()*2,'0');
        for(std::size_t i=0;i<digest.size();++i){hash[i*2]=digits[digest[i]>>4];hash[i*2+1]=digits[digest[i]&15];}
        std::lock_guard state_lock(state_mutex);
        visible_blocks.insert(std::move(hash));
      }
      if(number==5)std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      protowire::KaspadResponse response;response.set_id(number==8?request.id()+1:request.id());auto *result=response.mutable_submitblockresponse();
      if(number==2)result->set_rejectreason(protowire::SubmitBlockResponseMessage::IS_IN_IBD);
      else if(number==3){result->mutable_error()->set_message("mock node validation error");result->set_rejectreason(protowire::SubmitBlockResponseMessage::NONE);}
      else result->set_rejectreason(protowire::SubmitBlockResponseMessage::NONE);
      if(!stream->Write(response))return grpc::Status(grpc::StatusCode::UNAVAILABLE,"mock response write failed");
      continue;
    }
    return grpc::Status::OK;
  }
};
template<typename F>bool rejected(F&&f,const std::string&needle){try{f();}catch(const std::invalid_argument&e){return std::string(e.what()).find(needle)!=std::string::npos;}return false;}

int main(int argc,char**argv){
  try{
    if(argc!=9)throw std::runtime_error("usage: MODEL TREE METADATA CAPTURE_DIR ORACLE OUTPUT_DIR PAY_ADDRESS EXTRA_DATA");
    const fs::path model_path=argv[1],tree=argv[2],metadata=argv[3],capture_dir=argv[4],oracle=argv[5],out=argv[6];
    const std::string payout=argv[7],extra=argv[8];fs::create_directories(out);
    model::ReadOnlyGguf model(model_path);const auto sidecar=model.verify_sidecar(tree,metadata,official_model,official_root,203469888);
    if(sidecar.declared_root!=official_root)throw std::runtime_error("GGUF sidecar root mismatch");
    model::SparseMerkleIndex merkle(model,tree);
    protowire::RpcBlock base;protowire::KaspadResponse base_response;bool found=false;
    for(const auto&entry:fs::directory_iterator(capture_dir)){
      if(!entry.is_regular_file()||entry.path().extension()!=".capture")continue;
      for(const auto&record:read_capture(entry.path().string())){
        if(record.network!="mainnet")continue;protowire::KaspadResponse response;
        if(!response.ParseFromArray(record.response_wire.data(),static_cast<int>(record.response_wire.size()))||!response.has_getblocktemplateresponse())continue;
        const auto&r=response.getblocktemplateresponse();if(!r.issynced()||!r.has_block()||!r.block().has_header()||r.block().transactions_size()==0)continue;
        const auto h=header_from_rpc(r.block().header());if(h.daa_score<h14_activation_daa||h.pom_tier>4||official_tier_anchor(h.daa_score,h.pom_tier).root!=official_root)continue;
        base=r.block();base_response=std::move(response);found=true;break;
      }if(found)break;
    }
    if(!found)throw std::runtime_error("no complete synced mainnet capture matches the installed GGUF tier");
    const auto seed_nonce=0x4b45525958535542ULL;
    std::vector<Fixture>fixtures;fixtures.reserve(10);
    for(std::uint64_t i=0;i<10;++i)fixtures.push_back(make_fixture(model,merkle,base,base_response,oracle,out,seed_nonce+i,payout,extra));

    SubmitMock mock;
    for(const auto&f:fixtures){const auto assembled=network::assemble_pom_winner(f.template_block,f.winner,payout,extra);mock.expected_blocks.emplace(f.winner.verification.nonce,assembled.SerializeAsString());
      const auto hash=block_hash_for_daa(header_from_rpc(assembled.header()),assembled.header().nonce(),static_cast<std::uint64_t>(assembled.header().timestamp()));
      static constexpr char hex_digits[]="0123456789abcdef";std::string block_hash(hash.size()*2,'0');
      for(std::size_t n=0;n<hash.size();++n){block_hash[n*2]=hex_digits[hash[n]>>4];block_hash[n*2+1]=hex_digits[hash[n]&15];}
      auto indexed=assembled;auto *verbose=indexed.mutable_verbosedata();verbose->set_hash(block_hash);verbose->set_bluescore(assembled.header().bluescore());verbose->set_ischainblock(true);
      mock.blocks[block_hash]=std::move(indexed);mock.chain_patterns[block_hash]={true};
      if(assembled.pomproof()!=f.winner.borsh_proof||assembled.header().pomfinalstate()!=f.winner.final_state||
         assembled.header().pomtier()!=f.winner.tier||assembled.header().servicestatehash()!=f.template_block.header().servicestatehash()||
         assembled.transactions_size()!=f.template_block.transactions_size())throw std::runtime_error("complete block assembly omitted an H3/H6/template field");
      for(int tx=0;tx<f.template_block.transactions_size();++tx)if(assembled.transactions(tx).SerializeAsString()!=f.template_block.transactions(tx).SerializeAsString())throw std::runtime_error("payout/escrow transaction changed during assembly");}
    protowire::RpcBlock sink; sink.mutable_header()->set_bluescore(fixtures[0].template_block.header().bluescore()+20);
    sink.mutable_verbosedata()->set_hash(mock.sink_hash);sink.mutable_verbosedata()->set_bluescore(fixtures[0].template_block.header().bluescore()+20);
    sink.mutable_verbosedata()->set_ischainblock(true);mock.sink_blue_score=sink.verbosedata().bluescore();mock.blocks[mock.sink_hash]=sink;mock.visible_blocks.insert(mock.sink_hash);
    grpc::ServerBuilder builder;int port=0;builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);builder.RegisterService(&mock);auto server=builder.BuildAndStart();
    if(!server||!port)throw std::runtime_error("could not start loopback SubmitBlock mock");
    network::SubmitBlockClient client("grpc://127.0.0.1:"+std::to_string(port));
    const auto positive=network::submit_pom_winner(client,fixtures[0].template_block,fixtures[0].winner,payout,extra,true);
    if(positive.state!=network::SubmissionState::RpcAcknowledged||positive.transitions.size()!=3||positive.reservation.empty()||mock.requests.load()!=1)throw std::runtime_error("valid Rust-verified proof did not return RPC_ACKNOWLEDGED through production SubmitBlockClient helper");
    const auto &timing=positive.timing;
    for(const auto &v:{timing.assembly_ms,timing.validation_ms,timing.connect_ms,timing.stream_ms,timing.prepare_ms,timing.write_ms,timing.response_ms,timing.finish_ms,timing.total_ms})
      if(!v||!std::isfinite(*v)||*v<0)throw std::runtime_error("submission monotonic timing missing or invalid");
    if(timing.channel_reused)throw std::runtime_error("first submission incorrectly marked channel reuse");
    if(std::find(positive.transitions.begin(),positive.transitions.end(),network::SubmissionState::ChainConfirmed)!=positive.transitions.end())throw std::runtime_error("SubmitBlock RPC acknowledgement was mislabeled as chain confirmation");
    auto confirmed=client.observe_chain(positive,std::chrono::seconds(1),std::chrono::milliseconds(2),6);
    if(confirmed.state!=network::SubmissionState::ChainConfirmed||!confirmed.currently_selected_chain||confirmed.selected_tip_blue_score-confirmed.block_blue_score<6)throw std::runtime_error("GetBlock DAG and selected-chain confirmation policy failed");
    if(!rejected([&]{(void)network::submit_pom_winner(client,fixtures[0].template_block,fixtures[0].winner,payout,extra,true);},"duplicate"))throw std::runtime_error("duplicate candidate was not deduplicated");
    auto above=fixtures[0].winner;above.verification.target_satisfied=false;
    if(!rejected([&]{(void)network::submit_pom_winner(client,fixtures[0].template_block,above,payout,extra,true);},"target"))throw std::runtime_error("above-target candidate was not rejected");
    auto stale=fixtures[0].winner;stale.verification.job_fresh=false;
    if(!rejected([&]{(void)network::submit_pom_winner(client,fixtures[0].template_block,stale,payout,extra,true);},"stale"))throw std::runtime_error("stale candidate was not rejected");
    auto invalid=fixtures[0].winner;invalid.verification.pom_verified=false;
    if(!rejected([&]{(void)network::submit_pom_winner(client,fixtures[0].template_block,invalid,payout,extra,true);},"PoM proof"))throw std::runtime_error("invalid proof evidence was not rejected");
    auto disabled=fixtures[1].winner;
    if(!rejected([&]{(void)network::submit_pom_winner(client,fixtures[1].template_block,disabled,payout,extra,false);},"disabled"))throw std::runtime_error("submit-disabled mode contacted mock");
    const auto ibd=network::submit_pom_winner(client,fixtures[1].template_block,fixtures[1].winner,payout,extra,true);
    if(!ibd.timing.channel_reused||!ibd.timing.total_ms||!ibd.timing.response_ms)throw std::runtime_error("reused-channel reject timing unavailable");
    if(ibd.state!=network::SubmissionState::RpcRejected||ibd.reason!=protowire::SubmitBlockResponseMessage::IS_IN_IBD)throw std::runtime_error("node IBD reject was not preserved as RPC_REJECTED");
    if(!ibd.timing.stream_reused)throw std::runtime_error("second request did not reuse persistent stream");
    const auto rpc=network::submit_pom_winner(client,fixtures[2].template_block,fixtures[2].winner,payout,extra,true);
    if(rpc.state!=network::SubmissionState::RpcRejected||rpc.node_error!="mock node validation error")throw std::runtime_error("RPCError response was misreported");
    const auto disconnected=network::submit_pom_winner(client,fixtures[3].template_block,fixtures[3].winner,payout,extra,true,std::chrono::seconds(2));
    if(disconnected.state!=network::SubmissionState::UnknownOutcome)throw std::runtime_error("post-send disconnect was not represented as an unknown outcome");
    if(!rejected([&]{(void)network::submit_pom_winner(client,fixtures[3].template_block,fixtures[3].winner,payout,extra,true);},"duplicate"))throw std::runtime_error("ambiguous submit retry was not deduplicated");
    const auto timed_out=network::submit_pom_winner(client,fixtures[4].template_block,fixtures[4].winner,payout,extra,true,std::chrono::seconds(1));
    if(timed_out.state!=network::SubmissionState::UnknownAfterTimeout)throw std::runtime_error("SubmitBlock timeout was not represented as UNKNOWN_AFTER_TIMEOUT");
    const auto timeout_recovery=client.observe_chain(timed_out,std::chrono::seconds(1),std::chrono::milliseconds(2),6);
    if(timeout_recovery.state!=network::SubmissionState::ChainConfirmed)throw std::runtime_error("timed-out submission could not be reconciled by block hash without retry");
    const auto reorg_block=network::assemble_pom_winner(fixtures[5].template_block,fixtures[5].winner,payout,extra);
    const auto reorg_hash=block_hash_for_daa(header_from_rpc(reorg_block.header()),reorg_block.header().nonce(),static_cast<std::uint64_t>(reorg_block.header().timestamp()));
    static constexpr char reorg_hex_digits[]="0123456789abcdef";std::string reorg_hex(reorg_hash.size()*2,'0');
    for(std::size_t n=0;n<reorg_hash.size();++n){reorg_hex[n*2]=reorg_hex_digits[reorg_hash[n]>>4];reorg_hex[n*2+1]=reorg_hex_digits[reorg_hash[n]&15];}
    mock.chain_patterns[reorg_hex]={true,false};
    auto reorg_sink=mock.blocks.at(mock.sink_hash);reorg_sink.mutable_verbosedata()->set_bluescore(fixtures[5].template_block.header().bluescore()+2);mock.blocks[mock.sink_hash]=reorg_sink;
    const auto reorg_submit=network::submit_pom_winner(client,fixtures[5].template_block,fixtures[5].winner,payout,extra,true);
    if(reorg_submit.state!=network::SubmissionState::RpcAcknowledged)throw std::runtime_error("reorg fixture SubmitBlock was not RPC acknowledged");
    const auto reorg=client.observe_chain(reorg_submit,std::chrono::seconds(1),std::chrono::milliseconds(2),6);
    if(reorg.state!=network::SubmissionState::ChainObserved||!reorg.reorg_observed||reorg.currently_selected_chain)throw std::runtime_error("selected-chain exit/reorganization was not distinguished from DAG observation");
    if(mock.requests.load()!=6)throw std::runtime_error("target/stale/proof/duplicate/disabled gates sent an unexpected SubmitBlock request");
    std::atomic_bool ai_pending{false};
    std::jthread arrival([&]{const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      while(mock.requests.load()<7&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
      ai_pending=true;
    });
    const auto cancel_begin=std::chrono::steady_clock::now();
    const auto ai_cancelled=network::submit_pom_winner(client,fixtures[6].template_block,fixtures[6].winner,payout,extra,true,
        std::chrono::seconds(5),[&]{return ai_pending.load();});
    arrival.join();
    const auto cancel_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-cancel_begin).count();
    if(mock.requests.load()!=7||ai_cancelled.state!=network::SubmissionState::UnknownOutcome||cancel_ms>500)
      throw std::runtime_error("AI preemption did not promptly cancel the in-flight submission RPC");
    if(!rejected([&]{(void)network::submit_pom_winner(client,fixtures[6].template_block,fixtures[6].winner,payout,extra,true);},"duplicate"))
      throw std::runtime_error("AI-cancelled ambiguous submission was retried");
    std::cout<<"SUBMISSION_AI_PREEMPTION=PASS cancel_ms="<<cancel_ms<<" outcome=UNKNOWN_OUTCOME duplicate=BLOCKED\n";
    auto malformed=fixtures[0].template_block;malformed.clear_pomproof();
    if(!rejected([&]{(void)client.submit(malformed,fixtures[0].winner.verification,true);},"proof bytes"))throw std::runtime_error("malformed serialization was not rejected");
    const auto mismatch=network::submit_pom_winner(client,fixtures[7].template_block,fixtures[7].winner,payout,extra,true);
    if(mismatch.state!=network::SubmissionState::UnknownOutcome)throw std::runtime_error("correlation mismatch accepted");
    const auto recovered=network::submit_pom_winner(client,fixtures[8].template_block,fixtures[8].winner,payout,extra,true);
    if(recovered.state!=network::SubmissionState::RpcAcknowledged||recovered.timing.stream_reused)throw std::runtime_error("new candidate did not reconnect after broken stream");
    unsigned freshness_checks=0;
    fixtures[9].winner.verification.still_current=[&]{return ++freshness_checks==1;};
    const auto before_expiry=mock.requests.load();
    const auto expired=network::submit_pom_winner(client,fixtures[9].template_block,fixtures[9].winner,payout,extra,true);
    if(expired.state!=network::SubmissionState::CandidateValid||freshness_checks!=2||mock.requests.load()!=before_expiry)
      throw std::runtime_error("lease expiry before Write was not discarded");
    client.block_new_submissions();
    const auto before_audit_block=mock.requests.load();
    const auto blocked=network::submit_pom_winner(client,fixtures[8].template_block,fixtures[8].winner,payout,extra,true);
    if(blocked.state!=network::SubmissionState::CandidateValid||mock.requests.load()!=before_audit_block)
      throw std::runtime_error("sticky audit failure allowed a new SubmitBlock request");
    std::cout<<"PERSISTENT_SUBMIT_STREAM=PASS reuse=true correlation_mismatch=UNKNOWN reconnect_new_candidate=true\n";
    server->Shutdown(std::chrono::system_clock::now());
    std::cout<<"VALID_POM_PROOF_POSITIVE_MOCK=PASS source=authentic-GGUF CPU-walk Rust-official=PASS target=isolated-test-only bits=0x"<<std::hex<<easy_test_bits<<std::dec
      <<" full_block=header,nonce,pomProof,pomFinalState,serviceStateHash,pomTier,payout_escrow_transactions_preserved="<<fixtures[0].template_block.transactions_size()
      <<" production_helper_invoked=true mock_received_complete_message=true states=RPC_ACKNOWLEDGED,RPC_REJECTED,UNKNOWN_OUTCOME,UNKNOWN_AFTER_TIMEOUT,CHAIN_OBSERVED,CHAIN_CONFIRMED reorg_tracking=PASS gates=target,stale,proof,duplicate,disabled serialization=PASS mainnet_submit=DISABLED artifacts="<<out<<"\n";
    return 0;
  }catch(const std::exception&e){std::cerr<<"VALID_POM_PROOF_POSITIVE_MOCK=FAIL detail="<<e.what()<<" mainnet_submit=DISABLED\n";return 1;}
}
