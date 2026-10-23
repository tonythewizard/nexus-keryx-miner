#include "keryx/nexus/chain_notice.hpp"
#include "keryx/nexus/proof_artifact_buffers.hpp"
#include "keryx/nexus/rust_proof_verifier.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
using namespace keryx::nexus;
std::vector<std::uint8_t> read(const char *p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("missing fixture");return {std::istreambuf_iterator<char>(f),{}};}
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(int argc,char**argv) {
 try {
  ChainNoticeFilter notices;using R=ChainNoticeFilter::Result;
  require(notices.observe("a",true)==R::ChainChanged,"real change must invalidate");
  require(notices.observe("a",true)==R::Duplicate,"exact duplicate");
  require(notices.observe("b",true)==R::ChainChanged,"second reorg");
  require(notices.observe("a",true)==R::ChainChanged,"reverted branch cannot be suppressed");
  require(notices.observe("",false)==R::RefreshOnly,"empty notification must refresh");
  notices.clear();require(notices.observe("",false)==R::RefreshOnly,"new stream or intervening notification resets deduplication");
  ProofArtifactContext synthetic{};synthetic.seed=0x8877665544332211ULL;synthetic.generation=1;
  consensus::Walk synthetic_walk;synthetic_walk.states.resize(257);synthetic_walk.offsets.resize(256);
  for(unsigned i=0;i<257;++i)for(unsigned j=0;j<1024;++j)synthetic_walk.states[i][j]=(i+j)&255;
  for(unsigned i=0;i<256;++i)synthetic_walk.offsets[i]=0x0102030405060708ULL+i;
  auto encoded=encode_proof_artifacts(synthetic,synthetic_walk,17);
  require(encoded.context_prefix.size()==225 && encoded.trace.size()==265244,"bounded artifact sizes");
  require(encoded.context_prefix[4]==0x11 && encoded.context_prefix[11]==0x88 && encoded.context_prefix[221]==17,"context little endian");
  require(encoded.trace[28+1024+2]==3 && encoded.trace[28+257*1024]==8,"trace states and offsets");
  bool rejected=false;synthetic_walk.states.pop_back();try{encode_proof_artifacts(synthetic,synthetic_walk,17);}catch(...){rejected=true;}
  require(rejected,"invalid dimensions must be rejected");
  if(argc==1){std::cout<<"PROOF_ARTIFACT_BUFFERS=PASS CHAIN_NOTICES=PASS\n";return 0;}
  require(argc==5,"oracle context trace proof arguments required");
  auto c=read(argv[2]),t=read(argv[3]),p=read(argv[4]);
  require(c.size()==225+p.size() && t.size()==28+257*1024+256*8,"official artifact dimensions");
  std::size_t pos=4;
  auto le=[&](unsigned n){std::uint64_t x=0;for(unsigned i=0;i<n;++i)x|=std::uint64_t(c.at(pos++))<<(8*i);return x;};
  auto hash=[&]{consensus::Hash x{};std::copy_n(c.begin()+pos,32,x.begin());pos+=32;return x;};
  ProofArtifactContext a;a.seed=le(8);a.chunks=le(8);a.tier=le(1);a.root=hash();a.target=hash();a.pph=hash();a.timestamp=le(8);a.nonce=le(8);a.daa=le(8);a.generation=le(8);a.capture=hash();a.final=le(8);a.pow=hash();require(le(4)==p.size(),"wire dimensions");
  consensus::Walk walk;walk.states.resize(257);walk.offsets.resize(256);
  std::memcpy(walk.states.data(),t.data()+28,257*1024);
  for(unsigned i=0;i<256;++i){auto x=t.data()+28+257*1024+i*8;for(unsigned j=0;j<8;++j)walk.offsets[i]|=std::uint64_t(x[j])<<(8*j);}
  auto buffers=encode_proof_artifacts(a,walk,p.size());
  require(buffers.trace==t,"trace byte parity");require(std::equal(buffers.context_prefix.begin(),buffers.context_prefix.end(),c.begin()),"context byte parity");
  RustProofVerifier verifier(argv[1]);
  for(int i=0;i<8;++i){auto result=verifier.verify(buffers.context_prefix,buffers.trace,p);require(result.cpu_ms>=0,"CPU timing");}
  p[p.size()/2]^=1;bool failed=false;try{verifier.verify(buffers.context_prefix,buffers.trace,p);}catch(...){failed=true;}
  require(failed,"mutated proof must be rejected");failed=false;try{verifier.verify(buffers.context_prefix,buffers.trace,p);}catch(...){failed=true;}
  require(failed,"failed verifier cannot restart or retry");
  std::cout<<"PROOF_TRANSPORT=PASS byte_parity=true valid_frames=8 mutation_rejected=true failed_session_closed=true chain_notifications=PASS\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
