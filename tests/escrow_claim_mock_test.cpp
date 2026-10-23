#include "keryx/nexus/escrow_watcher.hpp"
#include "keryx/nexus/escrow_claim_policy.hpp"
#include "keryx/nexus/ai_service.hpp"
#include "keryx/nexus/ai_response_service.hpp"
#include "messages.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <atomic>
#include <unistd.h>
using namespace keryx::nexus::ai;
std::string hex(const std::array<std::uint8_t,32>& x){std::ostringstream s;for(auto b:x)s<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);return s.str();}
void require(bool b,const char *why){if(!b)throw std::runtime_error(why);}
class Mock final: public protowire::RPC::Service {
 public:
 protowire::GetUtxosByAddressesResponseMessage live;
 std::atomic_int queries{0};
 std::atomic_int calls{0};
 std::atomic_int mode{0};
 std::atomic_int anchor_mode{0};
 grpc::Status MessageStream(grpc::ServerContext*,grpc::ServerReaderWriter<protowire::KaspadResponse,protowire::KaspadRequest>* stream) override {
  protowire::KaspadRequest q;
  while(stream->Read(&q)) {
   if(q.has_getinforequest()||q.has_getblockdaginforequest()) {
    protowire::KaspadResponse r;r.set_id(anchor_mode==2?q.id()+1:q.id());
    if(q.has_getinforequest())r.mutable_getinforesponse()->set_issynced(anchor_mode!=1);
    else if(anchor_mode==3)r.mutable_getblockdaginforesponse()->mutable_error()->set_message("mock DAA unavailable");
    else if(anchor_mode!=4)r.mutable_getblockdaginforesponse()->set_virtualdaascore(100792010);
    stream->Write(r);continue;
   }
   if(!q.has_submittransactionrequest()) {
    require(q.has_getutxoentriesbyoutpointsrequest()&&q.getutxoentriesbyoutpointsrequest().outpoints_size()==87,"wrong UTXO request");
    protowire::KaspadResponse response;response.set_id(q.id());
    require(response.mutable_getutxoentriesbyoutpointsresponse()->ParseFromString(live.SerializeAsString()),"UTXO mock encode failed");stream->Write(response);++queries;continue;
   }
   require(q.has_submittransactionrequest(),"unexpected mock RPC");auto tx=q.submittransactionrequest().transaction();
   require(tx.inputs_size()==87&&tx.outputs_size()==1,"stock full claim batch missing");
   require(tx.outputs(0).amount()==87ULL*100000000-30000000,"claim fee/value changed");
   for(const auto &in:tx.inputs())require(in.sequence()==792000&&in.signaturescript().size()==132&&in.sigopcount()==1,"official CSV/signature encoding changed");
   auto id=consensus_transaction_id(tx);require(id.has_value(),"canonical claim ID failed");
   protowire::KaspadResponse r;r.set_id(q.id());
   if(mode==1)r.mutable_submittransactionresponse()->mutable_error()->set_message("isolated mock rejection");
   else r.mutable_submittransactionresponse()->set_transactionid(mode==2?std::string(64,'f'):hex(*id));++calls;
   if(!stream->Write(r))break;
  }
  return grpc::Status::OK;
 }
};
int main(int argc,char**argv){try{
 if(argc==3&&std::string_view(argv[1])=="--utxo-read-only") {
  std::ifstream in(argv[2]);require(bool(in),"outpoint list unavailable");std::vector<protowire::RpcOutpoint> ops;std::string txid;unsigned index;
  while(in>>txid>>index){require(txid.size()==64,"invalid outpoint ID");protowire::RpcOutpoint op;op.set_transactionid(txid);op.set_index(index);ops.push_back(op);}
  const auto live=query_escrow_utxos("grpc://127.0.0.1:22110",ops);
  std::cout<<"UTXO_READ_ONLY queried="<<ops.size()<<" live="<<live.entries_size()<<" absent="<<(ops.size()-live.entries_size())<<"\n";
  for(const auto &e:live.entries())std::cout<<e.outpoint().transactionid()<<' '<<e.outpoint().index()<<' '<<e.utxoentry().amount()<<' '<<e.utxoentry().blockdaascore()<<' '<<e.utxoentry().iscoinbase()<<' '<<e.utxoentry().scriptpublickey().version()<<' '<<e.utxoentry().scriptpublickey().scriptpublickey()<<'\n';
  return 0;
 }

 require(argc==2,"persistent fixture directory required");std::filesystem::path root=std::filesystem::path(argv[1])/std::to_string(getpid());
 require(!root.string().starts_with("/tmp"),"temporary path forbidden");std::filesystem::create_directories(root);
 require(!escrow_claim_check_due(5,6,5000,0),"monotonic boundary lost");
 require(!escrow_claim_check_due(6,6,2799,1000)&&escrow_claim_check_due(6,6,2800,1000),"30 minute cadence broken");
 require(!escrow_claim_check_due(6,6,999,1000),"clock rollback permitted claim");
 const auto state=root/"escrow.json";
 std::ofstream f(state);f<<"{\"entries\":[";
 for(unsigned i=0;i<87;++i){if(i)f<<',';std::ostringstream id;id<<std::hex<<std::setw(64)<<std::setfill('0')<<i+1;
  f<<"{\"coinbase_txid\":\""<<id.str()<<"\",\"block_hash\":\"\",\"confirm_daa\":100000000,\"amount_sompi\":100000000,\"output_index\":1,\"claimed\":false,\"slashed\":false,\"csv_window\":792000}";}
 f<<"],\"journal_seq\":0}";f.close();std::ifstream saved(state);const std::string initial((std::istreambuf_iterator<char>(saved)),std::istreambuf_iterator<char>());
 std::array<std::uint8_t,32> secret;secret.fill(0x11);
 EscrowWatcher watcher(secret,"keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr",state);
 require(watcher.start_state_validation().empty(),"fixture unexpectedly requires real chain state");
 protowire::RpcBlock block;block.mutable_header()->set_daascore(100792010);
 require(!watcher.handle_block(block).claim,"default policy spent fixture escrow");
 watcher.set_claim_construction_enabled(true);block.mutable_header()->set_daascore(100792009);
 require(!watcher.handle_block(block).claim,"premature CSV claim built");
 block.mutable_header()->set_daascore(100792010);auto built=watcher.handle_block(block);require(built.claim.has_value(),"mature stock claim missing");
 auto id=consensus_transaction_id(*built.claim);require(id.has_value(),"claim ID unavailable");
 require(!watcher.handle_block(block).claim,"in-flight outputs double claimed");
 Mock mock;grpc::ServerBuilder builder;int port=0;builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);builder.RegisterService(&mock);auto server=builder.BuildAndStart();
 require(server&&port!=22110&&port>0,"mock endpoint unsafe");
 const auto endpoint="grpc://127.0.0.1:"+std::to_string(port);
 require(query_escrow_virtual_daa(endpoint)==100792010,"fresh virtual DAA unavailable");
 for(int mode=1;mode<=4;++mode){mock.anchor_mode=mode;bool denied=false;
  try{(void)query_escrow_virtual_daa(endpoint);}catch(const std::exception&){denied=true;}
  require(denied,"unsynced/uncorrelated/missing DAA accepted");}
 mock.anchor_mode=0;
 auto reply=submit_ai_response_transaction("grpc://127.0.0.1:"+std::to_string(port),*built.claim,hex(*id));
 require(reply.state==AiResponseEffects::SubmitResult::State::Acknowledged&&mock.calls==1,"mock claim ACK missing");
 require(watcher.submit_result(hex(*id),std::nullopt)==2,"C++ ID differs from exact Rust stock ID");
 require(!watcher.handle_block(block).claim,"ACKed stock outputs double claimed");watcher.flush();
 for(int failure_mode=1;failure_mode<=2;++failure_mode) {
  const auto path=root/("error-"+std::to_string(failure_mode)+".json");std::ofstream(path)<<initial;
  EscrowWatcher retry(secret,"keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr",path);retry.set_claim_construction_enabled(true);
  auto candidate=retry.handle_block(block);require(candidate.claim.has_value(),"rejection fixture claim missing");auto digest=consensus_transaction_id(*candidate.claim);
  mock.mode=failure_mode;auto result=submit_ai_response_transaction("grpc://127.0.0.1:"+std::to_string(port),*candidate.claim,hex(*digest));
  require(result.state==(failure_mode==1?AiResponseEffects::SubmitResult::State::Rejected:AiResponseEffects::SubmitResult::State::Unknown),"RPC failure misreported as success");
  require(!result.detail.empty()&&retry.submit_result(hex(*digest),result.detail)==1,"rejected/unknown claim not recoverable");
  require(retry.handle_block(block).pending_count==87,"rejected/unknown outputs marked claimed");retry.flush();
 }
 // Real v1.6.4 UTXO wire extension, with no mainnet writes.
 protowire::GetUtxosByAddressesResponseMessage live;
 auto pubkey=stock_escrow_pubkey_from_secret(secret);
 for(unsigned i=0;i<87;++i) {
  auto *e=live.add_entries();std::ostringstream txid;txid<<std::hex<<std::setw(64)<<std::setfill('0')<<i+1;
  e->mutable_outpoint()->set_transactionid(txid.str());e->mutable_outpoint()->set_index(1);
  auto *u=e->mutable_utxoentry();u->set_amount(100000000);u->set_blockdaascore(100000000);u->set_iscoinbase(true);
  u->mutable_scriptpublickey()->set_version(0);u->mutable_scriptpublickey()->set_scriptpublickey("03c0150cb120"+hex(pubkey)+"ac");
 }
 // Nominal construction uses distinct, owned, mature live UTXOs, not the
 // historical record count. Partial pools never become full batches.
 for(unsigned count: {0u,51u,53u,86u,87u}) {
  const auto path=root/("nominal-"+std::to_string(count)+".json");std::ofstream(path)<<initial;
  EscrowWatcher checked(secret,"keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr",path);
  auto subset=live;while(unsigned(subset.entries_size())>count)subset.mutable_entries()->RemoveLast();
  auto result=checked.verified_claim(block,subset);
  require(result.has_value()==(count==87),"nominal 87-input threshold violated");
  if(result)require(result->inputs_size()==87,"nominal batch size changed");
 }
 {
  auto larger=initial;const auto at=larger.find("],\"journal_seq\"");
  for(unsigned i=88;i<=89;++i){std::ostringstream txid;txid<<std::hex<<std::setw(64)<<std::setfill('0')<<i;
   const auto row=",{\"coinbase_txid\":\""+txid.str()+"\",\"block_hash\":\"\",\"confirm_daa\":100000000,\"amount_sompi\":100000000,\"output_index\":1,\"claimed\":false,\"slashed\":false,\"csv_window\":792000}";
   larger.insert(at,row);
  }
  const auto path=root/"nominal-89.json";std::ofstream(path)<<larger;
  EscrowWatcher checked(secret,"keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr",path);
  auto all=live;for(unsigned i=88;i<=89;++i){auto *e=all.add_entries();*e=live.entries(0);std::ostringstream id;id<<std::hex<<std::setw(64)<<std::setfill('0')<<i;e->mutable_outpoint()->set_transactionid(id.str());}
  auto result=checked.verified_claim(block,all);
  require(result&&result->inputs_size()==87,"larger pool exceeded nominal 87 cap");
  require(!checked.verified_claim(block,all),"two leftover outputs created partial nominal claim");
 }
 {
  const auto path=root/"stale-notification.json";std::ofstream(path)<<initial;
  EscrowWatcher checked(secret,"keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr",path);
  auto selection=block;selection.mutable_header()->set_daascore(100792009);
  require(!checked.verified_claim(selection,live),"immature notification bypassed margin");
  selection.mutable_header()->set_daascore(query_escrow_virtual_daa(endpoint));
  require(checked.verified_claim(selection,live).has_value(),"fresh virtual DAA did not recover mature batch");
 }
 for(int kind=0;kind<10;++kind) {
  const auto path=root/("utxo-"+std::to_string(kind)+".json");std::ofstream(path)<<initial;
  EscrowWatcher checked(secret,"keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr",path);
  auto ops=checked.claim_outpoints(block.header().daascore());require(ops.size()==87,"pending outpoint list incorrect");
  auto response=live;
  if(kind==1)response.clear_entries();
  if(kind==2)response.mutable_entries(0)->mutable_utxoentry()->set_amount(1);
  if(kind==3)response.mutable_entries(0)->mutable_utxoentry()->mutable_scriptpublickey()->set_scriptpublickey("00");
  if(kind==4)response.mutable_entries(0)->mutable_utxoentry()->set_blockdaascore(100000001);
  if(kind==5)response.mutable_entries(0)->mutable_utxoentry()->set_iscoinbase(false);
  if(kind==6)response.mutable_error()->set_message("IBD transitional");
  if(kind==7)response.mutable_entries(0)->mutable_outpoint()->set_index(2);
  if(kind==8)response.mutable_entries(0)->mutable_utxoentry()->mutable_scriptpublickey()->set_version(1);
  if(kind==9)response.mutable_entries(0)->mutable_utxoentry()->mutable_scriptpublickey()->set_scriptpublickey("03c0150cb120"+std::string(64,'f')+"ac");
  auto tx=checked.verified_claim(block,response);
  require(tx.has_value()==(kind==0),"UTXO identity/amount/script/maturity gate failed");
  require(checked.handle_block(block).pending_count==87,"UTXO check deleted original records");
 }
 {
  auto pruned=initial;const std::string ghost(64,'a');const std::string field="\"block_hash\":\"\"";std::size_t pos=0;
  while((pos=pruned.find(field,pos))!=std::string::npos){auto replacement="\"block_hash\":\""+ghost+"\"";pruned.replace(pos,field.size(),replacement);pos+=replacement.size();}
  const auto path=root/"pruned.json";std::ofstream(path)<<pruned;
  EscrowWatcher pruned_watcher(secret,"keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr",path);
  require(pruned_watcher.start_state_validation().size()==1,"pruned fixture missing validation");
  require(pruned_watcher.validation_error("cannot find header "+ghost),"pruned unknown not correlated");
  pruned_watcher.set_claim_construction_enabled(true);
  require(!pruned_watcher.handle_block(block).claim,"unknown block bypassed without UTXO evidence");
  pruned_watcher.set_claim_construction_enabled(false);
  require(pruned_watcher.verified_claim(block,live).has_value(),"live verified UTXO blocked by pruned history");
 }
 mock.live=live;
 std::vector<protowire::RpcOutpoint> ops;for(const auto &e:live.entries())ops.push_back(e.outpoint());
 auto fetched=query_escrow_utxos("grpc://127.0.0.1:"+std::to_string(port),ops);
 require(fetched.entries_size()==87&&mock.queries==1,"official UTXO RPC wire failed");
 mock.live.add_entries()->CopyFrom(live.entries(0));bool denied=false;
 try{(void)query_escrow_utxos("grpc://127.0.0.1:"+std::to_string(port),ops);}catch(const std::exception&){denied=true;}
 require(denied,"duplicate UTXO response accepted");
 server->Shutdown();
 std::cout<<"ESCROW_CLAIM_MOCK=PASS signed_stock_batch=87 fee_sompi=30000000 csv_blocks=792000 cadence_sec=1800 default_deny=true premature_denied=true duplicate_denied=true rust_txid_match=true mainnet_transactions=0\n";
 return 0;
}catch(const std::exception&e){std::cerr<<"ESCROW_CLAIM_MOCK=FAIL "<<e.what()<<'\n';return 1;}}
