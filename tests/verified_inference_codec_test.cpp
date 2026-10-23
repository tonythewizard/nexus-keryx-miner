#include "keryx/nexus/ai_response_service.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
using namespace keryx::nexus::ai;
int main(int argc,char**argv) {try {
 std::array<std::uint8_t,32> request{},secret{};request.fill(9);secret.fill(7);
 std::array<std::uint8_t,34> cid{};cid[0]=0x12;cid[1]=0x20;
 std::string answer="verified inference compatibility";std::vector<std::uint8_t> body(40,3);
 const auto pubkey=stock_escrow_pubkey_from_secret(secret);
 for(bool sealed:{false,true}) {
  auto payload=stock_build_signed_response(request,secret,cid,125000000,
    {reinterpret_cast<const std::uint8_t*>(answer.data()),answer.size()},sealed?std::span<const std::uint8_t>(body):std::span<const std::uint8_t>{});
  if(argc==2){std::ofstream f(std::string(argv[1])+(sealed?".sealed":".public"),std::ios::binary);f.write(reinterpret_cast<const char*>(payload.data()),payload.size());if(!f)throw std::runtime_error("fixture write failed");}
  if(!sealed && !stock_verify_response(payload,request,pubkey,std::nullopt))throw std::runtime_error("legacy signature parity failed");
#ifdef NEXUS_TEST_TRACE_SUPPORTED
  TraceOpeningContext invalid_context{};
  if(stock_verify_trace_opening(payload,invalid_context))throw std::runtime_error("opening with unverified context accepted");
  TraceChainContext invalid_chain{};
  TracePrefixContext invalid_prefix{};
  if(stock_verify_trace_opening_chain(payload,invalid_context,invalid_prefix,invalid_chain))throw std::runtime_error("stage-2 opening with unverified context accepted");
  if(stock_verify_trace_opening_prefix(payload,invalid_context,invalid_prefix))throw std::runtime_error("prefix opening with unverified context accepted");
  if(stock_response_trace(payload))throw std::runtime_error("legacy path emitted a forbidden trace");
  if(sealed) {
   payload.push_back(0x81);payload.insert(payload.end(),{41,0,0,0});payload.insert(payload.end(),32,0x11);
   payload.insert(payload.end(),{7,0,0,0,11,0,0,0,1});
   auto trace=stock_response_trace(payload);
   if(!trace||trace->root[31]!=0x11||trace->prompt_positions!=7||trace->total_positions!=11||trace->stage!=1)throw std::runtime_error("native trace metadata differs from official codec");
   if(stock_verify_response(payload,request,pubkey,std::nullopt))throw std::runtime_error("modified signed trace accepted");
   payload.pop_back();if(stock_response_trace(payload))throw std::runtime_error("truncated trace accepted");
  }
#endif
 }
 std::cout<<"VERIFIED_INFERENCE_CODEC=PASS legacy_signed_bytes=true no_production_trace=true\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
