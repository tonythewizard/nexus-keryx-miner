// Offline node fixture for the actual worker. It never forwards an RPC and
// rejects every transaction/block submission; the runner admits only shadow
// mode.
#include "messages.grpc.pb.h"
#include <cstdlib>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <string>
class Mock final : public protowire::RPC::Service {
  std::string audit_;

public:
  explicit Mock(std::string audit) : audit_(std::move(audit)) {}
  grpc::Status MessageStream(
      grpc::ServerContext *,
      grpc::ServerReaderWriter<protowire::KaspadResponse,
                               protowire::KaspadRequest> *s) override {
    protowire::KaspadRequest q;
    while (s->Read(&q)) {
      protowire::KaspadResponse r;
      r.set_id(q.id());
      switch (q.payload_case()) {
      case protowire::KaspadRequest::kGetCurrentNetworkRequest:
        r.mutable_getcurrentnetworkresponse()->set_currentnetwork("mainnet");
        break;
      case protowire::KaspadRequest::kGetInfoRequest:
        r.mutable_getinforesponse()->set_issynced(true);
        r.mutable_getinforesponse()->set_serverversion(
            std::getenv("NEXUS_TEST_OLD_NODE") ? "1.6.5" : "1.6.6");
        break;
      case protowire::KaspadRequest::kGetServiceProvidersRequest:
        r.mutable_getserviceprovidersresponse()->set_virtualdaascore(140115000);
        break;
      case protowire::KaspadRequest::kGetAwaitedOpeningsRequest:
        if (std::getenv("NEXUS_TEST_OLD_NODE"))
          return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                              "old node must not receive ledger polls");
        r.mutable_getawaitedopeningsresponse()->set_virtualdaascore(140115000);
        break;
      case protowire::KaspadRequest::kGetServiceStrikesRequest:
        r.mutable_getservicestrikesresponse();
        break;
      case protowire::KaspadRequest::kGetBlockDagInfoRequest: {
        auto *p = r.mutable_getblockdaginforesponse();
        p->set_networkname("keryx-mainnet");
        p->set_virtualdaascore(140115000);
        p->set_sink(std::string(64, 'e'));
        break;
      }
      case protowire::KaspadRequest::kNotifyNewBlockTemplateRequest:
        r.mutable_notifynewblocktemplateresponse();
        break;
      case protowire::KaspadRequest::kNotifyVirtualChainChangedRequest:
        r.mutable_notifyvirtualchainchangedresponse();
        break;
      case protowire::KaspadRequest::kNotifyBlockAddedRequest:
        r.mutable_notifyblockaddedresponse();
        break;
      case protowire::KaspadRequest::kGetBlockTemplateRequest: {
        auto *p = r.mutable_getblocktemplateresponse();
        p->set_issynced(true);
        auto *h = p->mutable_block()->mutable_header();
        h->set_version(1);
        h->set_hashmerkleroot(std::string(64, 'a'));
        h->set_acceptedidmerkleroot(std::string(64, 'b'));
        h->set_utxocommitment(std::string(64, 'c'));
        h->set_pruningpoint(std::string(64, 'd'));
        h->set_bluework("01");
        h->set_timestamp(1792950000000);
        h->set_bits(0x1d00ffff);
        h->set_daascore(140115000);
        h->set_pomtier(0);
        h->add_parents()->add_parenthashes(std::string(64, 'e'));
        break;
      }
      case protowire::KaspadRequest::kGetBlockRequest: {
        auto *b = r.mutable_getblockresponse()->mutable_block();
        b->mutable_header()->set_daascore(140115000);
        b->mutable_verbosedata()->set_hash(q.getblockrequest().hash());
        b->mutable_verbosedata()->set_ischainblock(true);
        break;
      }
      case protowire::KaspadRequest::kGetVirtualChainFromBlockRequest:
        r.mutable_getvirtualchainfromblockresponse();
        break;
      case protowire::KaspadRequest::kSubmitTransactionRequest:
      case protowire::KaspadRequest::kSubmitBlockRequest: {
        std::ofstream f(audit_, std::ios::app);
        f << "FORBIDDEN_MUTATION\n";
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                            "offline shadow fixture forbids submissions");
      }
      default:
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                            "fixture RPC unsupported");
      }
      if (!s->Write(r))
        break;
      q.Clear();
    }
    return grpc::Status::OK;
  }
};
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  Mock mock(argv[1]);
  grpc::ServerBuilder b;
  int port = 0;
  b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  b.RegisterService(&mock);
  auto server = b.BuildAndStart();
  if (!server || !port)
    return 1;
  std::cout << "grpc://127.0.0.1:" << port << std::endl;
  server->Wait();
}
