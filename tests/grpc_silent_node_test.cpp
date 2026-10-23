#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/live_job_queue.hpp"
#include "keryx/nexus/grpc_runtime.hpp"
#include "messages.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

class SilentNode final : public protowire::RPC::Service {
public:
  explicit SilentNode(std::atomic_bool &stop):stop_(stop){}
  std::atomic_uint connections{0},probes{0},submissions{0};
  grpc::Status MessageStream(grpc::ServerContext *,grpc::ServerReaderWriter<protowire::KaspadResponse,protowire::KaspadRequest>*stream) override {
    protowire::KaspadRequest request;bool main_stream=false;
    while(stream->Read(&request)) {
      protowire::KaspadResponse response;response.set_id(request.id());
      switch(request.payload_case()) {
        case protowire::KaspadRequest::kGetCurrentNetworkRequest:
          main_stream=true;
          if(connections.fetch_add(1)+1>=2)stop_.store(true,std::memory_order_release);
          response.mutable_getcurrentnetworkresponse()->set_currentnetwork("Mainnet");break;
        case protowire::KaspadRequest::kGetInfoRequest:
          if(!main_stream)++probes;
          response.mutable_getinforesponse()->set_issynced(true);
          response.mutable_getinforesponse()->set_serverversion("local-silence-fixture");break;
        case protowire::KaspadRequest::kSubmitBlockRequest:
        case protowire::KaspadRequest::kSubmitTransactionRequest:
          ++submissions;return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,"fixture forbids writes");
        default:continue; // transport and separate health RPCs work; mining stream goes silent
      }
      if(!stream->Write(response))break;
    }
    return grpc::Status::OK;
  }
private:std::atomic_bool &stop_;
};
int main(){try{
  keryx::nexus::network::GrpcRuntime runtime;
  std::atomic_bool stop{false},done{false};std::atomic_int rc{-1};
  SilentNode node(stop);grpc::ServerBuilder builder;int port=0;
  builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);builder.RegisterService(&node);
  auto server=builder.BuildAndStart();if(!server || !port)throw std::runtime_error("mock bind failed");
  keryx::nexus::LiveJobQueue queue;keryx::nexus::TemplateFields f;
  f.network="mainnet";f.node_synced=true;f.block_wire={1,2};f.consensus_identity_wire={1,2};f.response_wire={3,4};
  (void)queue.publish(f);auto initial=queue.wait_pop();
  const auto start=std::chrono::steady_clock::now();
  std::thread client([&]{try{rc.store(keryx::nexus::run_live_job_client("grpc://127.0.0.1:"+std::to_string(port),"fixture:no-wallet","fixture",queue,0,0,stop));}catch(...){rc.store(99);}done.store(true,std::memory_order_release);});
  const auto deadline=start+std::chrono::seconds(105);
  while(!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const bool timed_out=!done.load(std::memory_order_acquire);
  stop.store(true,std::memory_order_release);client.join();server->Shutdown();server->Wait();
  const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  if(timed_out || rc.load()!=0 || node.connections.load()!=2 || node.probes.load()<2 || node.submissions.load()!=0 || !initial || initial->lease.valid() || elapsed<90'000)
    throw std::runtime_error("silent stream failed to recover at the real 90s policy");
  std::cout<<"GRPC_SILENT_NODE_RECOVERY=PASS elapsed_ms="<<elapsed<<" connections="<<node.connections<<" successful_health_probes="<<node.probes<<" old_lease=INVALID submissions=0 gpu=NOT_USED\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
