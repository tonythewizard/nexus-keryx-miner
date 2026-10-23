#include "keryx/nexus/grpc_header.hpp"
#include <iostream>
#include <stdexcept>
using namespace keryx::nexus::consensus;
static std::string hex(const Hash& h){const char*d="0123456789abcdef";std::string s;for(auto b:h){s+=d[b>>4];s+=d[b&15];}return s;}
int main(){try{
 Header h;h.version=1;h.timestamp=123;h.nonce=77;h.bits=0x1d00ffff;h.daa_score=129000000;h.blue_score=88;
 h.blue_work_big_endian={1,2};h.task_root.fill(0x55);h.pom_final_state=42;h.service_state_hash.fill(0x33);h.pom_tier=2;
 if(hex(hash_header_override(h,77,123,true,true,false))!="1739e013811521ac19f4e454d32f30541d197e1179fca81714b2e130c064b0d3")throw std::runtime_error("legacy_block byte order/hash mismatch");
 if(hex(hash_header_override(h,0,0,false,false,false))!="c0f8e97991a5e12dd16df9aedb0788b3cd7dcbadad03ce609770ddd45282bd2a")throw std::runtime_error("legacy_pre byte order/hash mismatch");
 if(hex(hash_header_override(h,77,123,true,true,true))!="231a0ef33af0fe7c01aa4bdd0663d422184bea57d65a77d7d3a95e0c545d0f03")throw std::runtime_error("task_block byte order/hash mismatch");
 if(hex(hash_header_override(h,0,0,false,false,true))!="f6c23ff0940b63a728abbeb77e607d0277ab8013cfdcb13994223bdc897c5add")throw std::runtime_error("task_pre byte order/hash mismatch");
 const auto before=pre_pow_hash(h);h.task_root.fill(0xaa);
 if(pre_pow_hash(h)!=before)throw std::runtime_error("inactive task root altered current mainnet PoW");
 if(block_task_activation_daa!=UINT64_MAX)throw std::runtime_error("unexpected task activation");
 protowire::RpcBlockHeader rpc;rpc.set_hashmerkleroot(std::string(64,'0'));rpc.set_acceptedidmerkleroot(std::string(64,'0'));
 rpc.set_utxocommitment(std::string(64,'0'));rpc.set_pruningpoint(std::string(64,'0'));rpc.set_taskroot(std::string(64,'5'));
 if(header_from_rpc(rpc).task_root[0]!=0x55)throw std::runtime_error("RPC task root lost");
 bool rejected=false;rpc.set_taskroot("invalid");try{(void)header_from_rpc(rpc);}catch(const std::invalid_argument&){rejected=true;}
 if(!rejected)throw std::runtime_error("invalid task root accepted");
 std::cout<<"BLOCK_TASK_HEADER=PASS current_mainnet_unchanged=true reference=official_order_independent_blake2\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
