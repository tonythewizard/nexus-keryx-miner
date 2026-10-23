#include "keryx/nexus/ai_response_service.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
int main(int argc,char **argv) {
 try {
  if(argc<2)throw std::runtime_error("local API URL required");
  const std::string body="NEXUS public synthetic transport fixture; no secrets or mainnet transaction.\n";
  const auto bytes=std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());
  for(int i=0;i<3;++i) {
   const auto t=std::chrono::steady_clock::now();
   const auto cid=keryx::nexus::ai::ipfs_publish_and_readback(argv[1],bytes);
   if(cid[0]!=0x12 || cid[1]!=0x20)throw std::runtime_error("invalid CID multihash");
   std::cout<<"IPFS_TRANSPORT_PASS iteration="<<i<<" elapsed_ms="
     <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count()<<"\n";
  }
  for(const auto url:{"http://127.0.0.1:5001@other.invalid:5001","http://127.0.0.1:5001/api","https://127.0.0.1:5001","http://user@127.0.0.1:5001"}) {
   bool rejected=false;try {(void)keryx::nexus::ai::ipfs_publish_and_readback(url,bytes);}
   catch(const std::invalid_argument &){rejected=true;}
   if(!rejected)throw std::runtime_error("unsafe API URL accepted");
  }
  if(argc>2) {
   for(int i=2;i<argc;++i) {
    bool rejected=false;try{(void)keryx::nexus::ai::ipfs_publish_and_readback(argv[i],bytes);}
    catch(const std::runtime_error &){rejected=true;}
    if(!rejected)throw std::runtime_error("corrupt IPFS fixture accepted");
   }
  }
  std::cout<<"IPFS_TRANSPORT=PASS readback=true root_hash=true endpoint_checks=true mainnet_tx=0\n";
 }catch(const std::exception &e){std::cerr<<"IPFS_TRANSPORT=FAIL "<<e.what()<<"\n";return 1;}
}
