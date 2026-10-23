// Read-only escrow accounting. No signing, CUDA calls, journal writes or submissions.
#include "keryx/nexus/escrow_watcher.hpp"
#include "messages.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <limits>
struct Record {std::uint64_t amount,csv,daa,retry,cap;bool coinbase,orphan;};
std::string script(std::string key,std::uint64_t csv){
 std::string raw;do{raw.push_back(char(csv&255));csv>>=8;}while(csv);
 std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(2)<<raw.size();for(auto c:raw)out<<std::setw(2)<<unsigned(static_cast<unsigned char>(c));return out.str()+"b120"+key+"ac";
}
int main(){try{
 std::string key;if(!(std::cin>>key)||key.size()!=64||key.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("public escrow identity required");
 std::vector<protowire::RpcOutpoint> ops;std::unordered_map<std::string,Record> records;
 std::string tx;std::uint32_t index;Record rec{};
 while(std::cin>>tx>>index>>rec.amount>>rec.csv>>rec.daa>>rec.retry>>rec.coinbase>>rec.cap>>rec.orphan){
  if(ops.size()>=10000||tx.size()!=64||tx.find_first_not_of("0123456789abcdef")!=std::string::npos||(rec.csv!=36000&&rec.csv!=792000))throw std::runtime_error("invalid bounded accounting input");
  protowire::RpcOutpoint op;op.set_transactionid(tx);op.set_index(index);ops.push_back(op);
  if(!records.emplace(tx+":"+std::to_string(index),rec).second)throw std::runtime_error("duplicate accounting outpoint");
 }
 if(!std::cin.eof())throw std::runtime_error("malformed accounting input");
 auto stub=protowire::RPC::NewStub(grpc::CreateChannel("127.0.0.1:22110",grpc::InsecureChannelCredentials()));
 auto rpc=[&](protowire::KaspadRequest request){grpc::ClientContext ctx;ctx.set_deadline(std::chrono::system_clock::now()+std::chrono::seconds(5));auto stream=stub->MessageStream(&ctx);protowire::KaspadResponse response;
  request.set_id(1);if(!stream->Write(request)||!stream->WritesDone()||!stream->Read(&response)||response.id()!=1){ctx.TryCancel();throw std::runtime_error("accounting RPC unavailable");}
  if(!stream->Finish().ok())throw std::runtime_error("accounting RPC failed");return response;};
 protowire::KaspadRequest info;info.mutable_getinforequest();auto ir=rpc(info);
 if(!ir.has_getinforesponse()||ir.getinforesponse().has_error()||!ir.getinforesponse().issynced())throw std::runtime_error("node not synced");
 protowire::KaspadRequest dag;dag.mutable_getblockdaginforequest();auto dr=rpc(dag);
 if(!dr.has_getblockdaginforesponse()||dr.getblockdaginforesponse().has_error())throw std::runtime_error("node DAA unavailable");
 const auto daa=dr.getblockdaginforesponse().virtualdaascore();
 const auto live=keryx::nexus::ai::query_escrow_utxos("grpc://127.0.0.1:22110",ops);
 std::uint64_t count=0,amount=0,owned=0,cooling=0,nominal=0;
 // Same activation and nominal batch as the pinned stock selector.
 const auto current_csv=daa>=76316623ULL?792000ULL:36000ULL;
 const auto mature=[&](std::uint64_t at,std::uint64_t window){return at<=daa&&daa-at>=window+10;};
 for(const auto &e:live.entries()){
  auto r=records.at(e.outpoint().transactionid()+":"+std::to_string(e.outpoint().index()));const auto &u=e.utxoentry();
  if(u.amount()!=r.amount||u.iscoinbase()!=r.coinbase||u.scriptpublickey().version()!=0||u.scriptpublickey().scriptpublickey()!=script(key,r.csv))continue;
  ++owned;if(!mature(r.daa,r.csv)||!mature(u.blockdaascore(),r.csv))continue;
  if(daa<r.retry){++cooling;continue;}
  if(amount>std::numeric_limits<std::uint64_t>::max()-r.amount)throw std::runtime_error("accounting sum overflow");
  ++count;amount+=r.amount;
  if(r.csv==current_csv&&r.cap==0&&!r.orphan)++nominal;
 }
 std::cout<<"{\"schema_version\":2,\"status\":\"AVAILABLE\",\"daa\":"<<daa<<",\"queried\":"<<ops.size()<<",\"live\":"<<live.entries_size()<<",\"owned\":"<<owned<<",\"outputs\":"<<count<<",\"amount_sompi\":"<<amount<<",\"cooldown_outputs\":"<<cooling<<",\"nominal_outputs\":"<<nominal<<",\"nominal_batch\":87,\"nominal_csv\":"<<current_csv<<",\"basis\":\"verified UTXO, identity and maturity; gross; stock batch rules apply\"}\n";
 return 0;
}catch(const std::exception &e){std::cerr<<"ESCROW_STATUS=UNAVAILABLE "<<e.what()<<'\n';return 1;}}
