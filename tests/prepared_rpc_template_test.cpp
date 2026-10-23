#include "keryx/nexus/prepared_rpc_template.hpp"
#include "keryx/nexus/capture.hpp"
#include <chrono>
#include <iostream>
using namespace keryx::nexus;
static void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char **argv){try {
  if(argc!=2)throw std::runtime_error("recorded capture required");
  auto records=read_capture(argv[1]);require(!records.empty(),"empty capture");
  auto wire=records[0].response_wire;RpcTemplateCache baseline;PreparedRpcTemplateCache cache;
  auto verify=[&](const auto &bytes) {
    const auto &block=baseline.block(bytes);const auto header=consensus::header_from_rpc(block.header());
    const auto &work=cache.get(bytes);
    require(work.pre_pow==consensus::pre_pow_hash(header),"cached pre-PoW differs");
    require(work.target.little_endian==consensus::decode_compact_target(header.bits).little_endian,"cached target differs");
    require(cache.block(bytes).SerializeAsString()==block.SerializeAsString(),"cached block bytes differ");
  };
  verify(wire);const auto *owned=&cache.get(wire);for(unsigned i=0;i<1000;++i)require(&cache.get(wire)==owned,"same snapshot reallocated work");
  protowire::KaspadResponse response;require(response.ParseFromArray(wire.data(),wire.size()),"bad fixture");
  for(unsigned mode=0;mode<4;++mode){
    if(mode==0)response.set_id(response.id()+1);
    if(mode==1)response.mutable_getblocktemplateresponse()->mutable_block()->mutable_header()->set_timestamp(response.getblocktemplateresponse().block().header().timestamp()+1);
    if(mode==2)response.mutable_getblocktemplateresponse()->mutable_block()->mutable_header()->set_bits(0x1d00ffff);
    if(mode==3)response.mutable_getblocktemplateresponse()->mutable_block()->add_transactions()->set_payload("test-only replacement");
    const auto text=response.SerializeAsString();verify(std::vector<std::uint8_t>(text.begin(),text.end()));
  }
  for(const auto &bad:std::vector<std::vector<std::uint8_t>>{{},{255}}){bool rejected=false;try{cache.get(bad);}catch(const std::exception&){rejected=true;}require(rejected,"invalid snapshot accepted");verify(wire);}
  response.mutable_getblocktemplateresponse()->mutable_block()->mutable_header()->set_bits(0);auto text=response.SerializeAsString();bool zero=false;try{cache.get({text.begin(),text.end()});}catch(const std::exception&){zero=true;}require(zero,"zero target accepted");verify(wire);
  std::uint64_t sink=0;
  for(unsigned pair=0;pair<8;++pair)for(unsigned order=0;order<2;++order){const bool candidate=(pair+order)%2;constexpr unsigned n=3000;const auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<n;++i){if(candidate){cache.block(wire);const auto &w=cache.get(wire);sink+=w.pre_pow[0]+w.target.little_endian[0];}else{const auto &b=baseline.block(wire);auto h=consensus::header_from_rpc(b.header());sink+=consensus::pre_pow_hash(h)[0]+consensus::decode_compact_target(h.bits).little_endian[0];}}
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();std::cout<<"PREPARATION_MEASUREMENT pair="<<pair<<" candidate="<<candidate<<" ns_per_batch="<<ns/n<<'\n';
  }
  std::cout<<"PREPARED_TEMPLATE=PASS complete_wire_key=true same_generation_updates=true malformed_rejected=true zero_target_rejected=true sink="<<sink<<'\n';return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
