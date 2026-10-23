#pragma once
// Shared protocol-only fixtures. Include inside the test anonymous namespace
// after the standard/protobuf/gRPC headers. Never reads production identity.
constexpr std::string_view payout = "keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr";
constexpr std::string_view model_hex = "bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710";

std::vector<std::uint8_t> unhex(std::string_view s) {
  if ((s.size() & 1U) != 0) throw std::runtime_error("odd hex length");
  std::vector<std::uint8_t> out(s.size()/2);
  auto n=[](char c)->int { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; };
  for(size_t i=0;i<out.size();++i){int a=n(s[2*i]),b=n(s[2*i+1]);if(a<0||b<0)throw std::runtime_error("bad hex");out[i]=static_cast<std::uint8_t>((a<<4)|b);} return out;
}
std::string hex(std::span<const std::uint8_t> b){static constexpr char d[]="0123456789abcdef";std::string s(b.size()*2,'0');for(size_t i=0;i<b.size();++i){s[2*i]=d[b[i]>>4];s[2*i+1]=d[b[i]&15];}return s;}
std::array<std::uint8_t,34> mh(std::span<const std::uint8_t> b){std::array<std::uint8_t,34> out{};out[0]=0x12;out[1]=0x20;SHA256(b.data(),b.size(),out.data()+2);return out;}
std::string txid(const protowire::RpcTransaction &tx){std::string b;auto le=[&](std::uint64_t v,unsigned n){for(unsigned i=0;i<n;++i)b.push_back(static_cast<char>(v>>(8U*i)));};le(tx.version(),2);le(tx.inputs_size(),8);le(tx.outputs_size(),8);le(tx.locktime(),8);auto sub=unhex(tx.subnetworkid()),p=unhex(tx.payload());b.append(reinterpret_cast<const char*>(sub.data()),sub.size());le(tx.gas(),8);le(p.size(),8);b.append(reinterpret_cast<const char*>(p.data()),p.size());std::array<std::uint8_t,32> out{};constexpr char key[]="TransactionID";if(blake2b(out.data(),b.data(),key,32,b.size(),sizeof(key)-1)!=0)throw std::runtime_error("tx hash error");return hex(out);}
struct Fixture { protowire::RpcTransaction tx; std::array<std::uint8_t,32> root{},escrow{};std::array<std::uint8_t,64> cert{}; };
Fixture fixture(const fs::path &path){std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("fixture missing");std::array<std::uint8_t,4> n{};in.read(reinterpret_cast<char*>(n.data()),4);if(!in)throw std::runtime_error("fixture header");std::uint32_t len=0;for(unsigned i=0;i<4;++i)len|=static_cast<std::uint32_t>(n[i])<<(8*i);std::vector<std::uint8_t> payload(len);in.read(reinterpret_cast<char*>(payload.data()),len);Fixture f;in.read(reinterpret_cast<char*>(f.root.data()),32);in.read(reinterpret_cast<char*>(f.escrow.data()),32);in.read(reinterpret_cast<char*>(f.cert.data()),64);if(!in||in.peek()!=EOF)throw std::runtime_error("fixture size invalid");f.tx.set_version(0);f.tx.set_locktime(0);f.tx.set_gas(0);f.tx.set_subnetworkid("0300000000000000000000000000000000000000");f.tx.set_payload(hex(payload));return f;}

class SubmitMock final : public protowire::RPC::Service {
public:
 void expect_block(const protowire::RpcBlock &block){std::lock_guard l(m_);expected_block_=block.SerializeAsString();}
 unsigned block_count(){std::lock_guard l(m_);return blocks_;}
 void set_template_capture(const fs::path &path){
  const auto records=read_capture(path);bool found=false;
  for(const auto &record:records){if(record.network!="mainnet")continue;protowire::KaspadResponse response;
   if(!response.ParseFromArray(record.response_wire.data(),static_cast<int>(record.response_wire.size()))||!response.has_getblocktemplateresponse()||!response.getblocktemplateresponse().issynced())continue;
   template_response_=std::move(response);found=true;break;}
  if(!found)throw std::runtime_error("gRPC integration mock has no synced authentic mainnet template capture");
 }
 void set_ai_request(const protowire::RpcTransaction &tx,std::uint64_t daa){std::lock_guard l(m_);ai_tx_=tx;ai_daa_=daa;}
 grpc::Status MessageStream(grpc::ServerContext *context,grpc::ServerReaderWriter<protowire::KaspadResponse,protowire::KaspadRequest>* s) override {
  protowire::KaspadRequest q; while(s->Read(&q)){
   protowire::KaspadResponse r;r.set_id(q.id());
   switch(q.payload_case()){
    case protowire::KaspadRequest::kGetCurrentNetworkRequest:r.mutable_getcurrentnetworkresponse()->set_currentnetwork("Mainnet");if(!s->Write(r))return grpc::Status::OK;q.Clear();continue;
    case protowire::KaspadRequest::kGetInfoRequest:r.mutable_getinforesponse()->set_serverversion("isolated-ai-pom-mock");r.mutable_getinforesponse()->set_issynced(true);if(!s->Write(r))return grpc::Status::OK;q.Clear();continue;
    case protowire::KaspadRequest::kNotifyNewBlockTemplateRequest:r.mutable_notifynewblocktemplateresponse();if(!s->Write(r))return grpc::Status::OK;q.Clear();continue;
    case protowire::KaspadRequest::kNotifyVirtualChainChangedRequest:r.mutable_notifyvirtualchainchangedresponse();if(!s->Write(r))return grpc::Status::OK;q.Clear();continue;
    case protowire::KaspadRequest::kNotifyBlockAddedRequest:{r.mutable_notifyblockaddedresponse();if(!s->Write(r))return grpc::Status::OK;
      protowire::KaspadResponse note;auto *block=note.mutable_blockaddednotification()->mutable_block();protowire::RpcTransaction tx;std::uint64_t daa{};
      {std::lock_guard l(m_);tx=ai_tx_;daa=ai_daa_;}
      block->mutable_header()->set_daascore(daa);block->mutable_header()->add_parents()->add_parenthashes(std::string(64,'e'));block->add_transactions()->CopyFrom(tx);block->mutable_verbosedata()->set_hash(std::string(64,'d'));
      if(!s->Write(note))return grpc::Status::OK;q.Clear();continue;}
    case protowire::KaspadRequest::kGetBlockTemplateRequest:{if(!template_response_.has_getblocktemplateresponse())return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,"template capture not configured");
      r.CopyFrom(template_response_);r.set_id(q.id());if(!s->Write(r))return grpc::Status::OK;q.Clear();continue;}
    case protowire::KaspadRequest::kSubmitBlockRequest:{
      {std::lock_guard l(m_);
       if(expected_block_.empty()||q.submitblockrequest().block().SerializeAsString()!=expected_block_)
         return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,"unexpected loopback block");
       ++blocks_;}
      r.mutable_submitblockresponse()->set_rejectreason(protowire::SubmitBlockResponseMessage::NONE);
      if(!s->Write(r))return grpc::Status::OK;q.Clear();continue;}
    case protowire::KaspadRequest::kSubmitTransactionRequest:break;
    default:q.Clear();continue;
   }
   {std::lock_guard l(m_);peers_.insert(context->peer());}
   auto tx=q.submittransactionrequest().transaction();
   if(tx.subnetworkid()!="0400000000000000000000000000000000000000"||tx.inputs_size()!=0||tx.outputs_size()!=0)return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,"bad AiResponse tx");
   bool reject=false;{std::lock_guard l(m_);last_=tx;++count_;reject=reject_next_;reject_next_=false;} cv_.notify_all(); r.Clear();r.set_id(q.id());
   if(reject)r.mutable_submittransactionresponse()->mutable_error()->set_message("isolated mock rejection");
   else r.mutable_submittransactionresponse()->set_transactionid(txid(tx));
   if(!s->Write(r))break;q.Clear();
  } return grpc::Status::OK;
 }
 bool wait_count(int n){std::unique_lock l(m_);return cv_.wait_for(l,std::chrono::seconds(5),[&]{return count_>=n;});}
 int count(){std::lock_guard l(m_);return count_;}
 std::size_t peers(){std::lock_guard l(m_);return peers_.size();}
 protowire::RpcTransaction last(){std::lock_guard l(m_);return last_;}
 void reject_next(){std::lock_guard l(m_);reject_next_=true;}
private:std::mutex m_;std::unordered_set<std::string> peers_;std::condition_variable cv_;int count_{};bool reject_next_{};protowire::RpcTransaction last_,ai_tx_;std::uint64_t ai_daa_{};protowire::KaspadResponse template_response_;std::string expected_block_;unsigned blocks_{};
};

void require(bool ok,const char *m);

void discover_via_live_grpc(SubmitMock &mock,std::string endpoint,AiResponseService &service,
                           const protowire::RpcTransaction &expected,std::uint64_t daa){
 mock.set_ai_request(expected,daa);LiveJobQueue queue;std::atomic_bool stop{false};std::atomic_bool discovered{false};std::atomic_int rc{-99};
 std::thread client([&]{rc.store(run_live_job_client(endpoint,std::string(payout),"",queue,1,0,stop,{},
   [&](const protowire::RpcBlock &block,bool){if(!block.has_header())return;for(const auto &tx:block.transactions()){
     if(tx.subnetworkid()==expected.subnetworkid()&&tx.payload()==expected.payload()&&service.discover(tx,block.header().daascore()))discovered.store(true,std::memory_order_release);
   }}),std::memory_order_release);});
 const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
 while(!discovered.load(std::memory_order_acquire)&&rc.load(std::memory_order_acquire)==-99&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));
 if(!discovered.load(std::memory_order_acquire)){stop.store(true,std::memory_order_release);client.join();throw std::runtime_error("live gRPC BlockAdded fixture did not discover assigned AiRequest");}
 client.join();require(rc.load(std::memory_order_acquire)==0,"live gRPC AiRequest ingress did not shut down cleanly");
}

AiResponseConfig config_for(const Fixture &f,const fs::path &journal){
 AiResponseConfig c; c.escrow_secret.fill(0x22);c.escrow_pubkey=f.escrow;c.delegation_signature=f.cert;const auto model=unhex(model_hex);std::copy(model.begin(),model.end(),c.model_id.begin());c.payout_address=payout;c.network="mainnet";c.service_identity=*service_identity_from_payout(payout);c.tier=0;c.private_inline_activation_daa=mainnet_private_inference_gate_daa;c.signed_response_activation_daa=0;c.journal_path=journal;return c;
}
std::vector<ServiceProvider> providers_for(const Fixture &f){return {{0,std::string(model_hex),*service_identity_from_payout(payout),hex(f.escrow)}};}
AiResponseEffects effects_for(std::string endpoint,std::function<std::string(std::string_view,std::uint32_t)> infer,SubmitMock &mock,std::string ipfs_api={}){
 AiResponseEffects e;e.infer=std::move(infer);
 e.publish_and_readback=[ipfs_api=std::move(ipfs_api)](std::span<const std::uint8_t> b){return ipfs_api.empty()?mh(b):ipfs_publish_and_readback(ipfs_api,b);};
 auto transport=std::make_shared<AiResponseTransport>(endpoint);
 e.submit=[transport](const protowire::RpcTransaction &tx,std::string_view id){return transport->submit(tx,id);};e.reconcile=[](std::string_view){return false;};(void)mock;return e;
}
void require(bool ok,const char *m){if(!ok)throw std::runtime_error(m);}
