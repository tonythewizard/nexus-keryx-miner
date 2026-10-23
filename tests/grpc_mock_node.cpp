#include "messages.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include "keryx/nexus/capture.hpp"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <array>
#include <chrono>
#include <thread>

class Mock final : public protowire::RPC::Service {
 public:
  Mock(std::vector<std::uint8_t> response_wire, std::string network, bool hold,
       bool service_fixture, bool ai_block_fixture, bool chain_status_fixture, bool slow_strikes)
      : response_wire_(std::move(response_wire)), network_(std::move(network)), hold_(hold),
        service_fixture_(service_fixture), ai_block_fixture_(ai_block_fixture),
        chain_status_fixture_(chain_status_fixture), slow_strikes_(slow_strikes) {}
  grpc::Status MessageStream(grpc::ServerContext *,
      grpc::ServerReaderWriter<protowire::KaspadResponse, protowire::KaspadRequest> *stream) override {
    protowire::KaspadRequest request;
    std::vector<protowire::KaspadResponse> deferred_chain_responses;
    bool supplied_template=false;
    while (stream->Read(&request)) {
      bool close_after_ai_backfill = false;
      protowire::KaspadResponse response; response.set_id(request.id());
      switch (request.payload_case()) {
      case protowire::KaspadRequest::kGetCurrentNetworkRequest:
        response.mutable_getcurrentnetworkresponse()->set_currentnetwork("Mainnet"); break;
      case protowire::KaspadRequest::kGetInfoRequest:
        response.mutable_getinforesponse()->set_serverversion("mock-official-schema");
        response.mutable_getinforesponse()->set_issynced(true); break;
      case protowire::KaspadRequest::kGetServiceProvidersRequest:
        if (!service_fixture_) continue;
        if(request.getserviceprovidersrequest().windowdaa()==35999) {
          response.mutable_getserviceprovidersresponse()->mutable_error()->set_message("injected provider index unavailable");
          break;
        }
        if(request.getserviceprovidersrequest().windowdaa()==35998) continue;
        if(request.getserviceprovidersrequest().windowdaa()==35997)response.set_id(999);
        response.mutable_getserviceprovidersresponse()->set_virtualdaascore(120000100);
        {
          auto *provider = response.mutable_getserviceprovidersresponse()->add_providers();
          provider->set_tier(0); provider->set_modelid("bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710");
          provider->set_identity(std::string(64, 'b')); provider->set_escrowpubkey(std::string(64, 'a'));
        }
        break;
      case protowire::KaspadRequest::kGetServiceStrikesRequest:
        if (!service_fixture_) continue;
        if (slow_strikes_) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        response.mutable_getservicestrikesresponse()->set_virtualdaascore(120000100);
        {
          auto *strike = response.mutable_getservicestrikesresponse()->add_strikes();
          strike->set_miner(std::string(64, 'b')); strike->set_consecutivemisses(2); strike->set_laststrikedaascore(120000050);
          auto *total = response.mutable_getservicestrikesresponse()->add_lifetimestrikes();
          total->set_miner(std::string(64, 'b')); total->set_strikes(7);
          auto *burn = response.mutable_getservicestrikesresponse()->add_pendingburns();
          burn->set_miner(std::string(64, 'b')); burn->set_missdaascore(120000040);
          burn->set_burnedclaims(1); burn->set_burnedsompi(2500000);
          auto *suspension = response.mutable_getservicestrikesresponse()->add_suspended();
          suspension->set_miner(std::string(64, 'b')); suspension->set_untildaascore(120001000);
        }
        break;
      case protowire::KaspadRequest::kNotifyNewBlockTemplateRequest:
        response.mutable_notifynewblocktemplateresponse(); break;
      case protowire::KaspadRequest::kNotifyVirtualChainChangedRequest:
        response.mutable_notifyvirtualchainchangedresponse(); break;
      case protowire::KaspadRequest::kNotifyBlockAddedRequest: {
        response.mutable_notifyblockaddedresponse();
        if (!stream->Write(response)) return grpc::Status::OK;
        if (ai_block_fixture_) {
          protowire::KaspadResponse notification;
          auto *block = notification.mutable_blockaddednotification()->mutable_block();
          block->mutable_header()->set_daascore(120'000'001);
          block->mutable_header()->add_parents()->add_parenthashes(std::string(64, 'e'));
          block->mutable_verbosedata()->set_hash(std::string(64, 'd'));
          if (!stream->Write(notification)) return grpc::Status::OK;
        }
        request.Clear();
        continue;
      }
      case protowire::KaspadRequest::kGetBlockDagInfoRequest:
        if (ai_block_fixture_) {
          auto *dag = response.mutable_getblockdaginforesponse();
          dag->set_virtualdaascore(120'000'001);
          dag->add_virtualparenthashes(std::string(64, 'e'));
        } else {
          continue;
        }
        break;
      case protowire::KaspadRequest::kGetBlockRequest: {
        if (chain_status_fixture_) {
          const auto &get_block = request.getblockrequest();
          const auto hash = get_block.hash();
          if (hash == std::string(64, '3')) {
            response.mutable_getblockresponse()->mutable_error()->set_message("fixture RPC protocol failure");
          } else if (hash == std::string(64, '6')) {
            response.mutable_getblockresponse()->mutable_error()->set_message("cannot find header " + hash);
          } else if (hash == std::string(64, '5')) {
            response.mutable_getblockresponse()->mutable_block();
          } else {
            auto *fetched = response.mutable_getblockresponse()->mutable_block();
            if (hash != std::string(64, '4')) {
              fetched->mutable_verbosedata()->set_hash(hash);
              fetched->mutable_verbosedata()->set_ischainblock(hash == std::string(64, '1'));
            } else {
              fetched->mutable_verbosedata()->set_hash(hash);
              // Deliberately leave optional isChainBlock absent: this is UNKNOWN, not false.
            }
          }
          deferred_chain_responses.push_back(std::move(response));
          request.Clear();
          continue;
        } else if (ai_block_fixture_) {
          auto *fetched = response.mutable_getblockresponse()->mutable_block();
          const auto &get_block = request.getblockrequest();
          const bool parent_backfill = get_block.hash() == std::string(64, 'e');
          close_after_ai_backfill = parent_backfill;
          fetched->mutable_header()->set_daascore(parent_backfill ? 120'000'000 : 120'000'001);
          fetched->mutable_verbosedata()->set_hash(get_block.hash());
          if (parent_backfill) {
            auto *tx = fetched->add_transactions();
            tx->set_subnetworkid("0300000000000000000000000000000000000000");
            std::vector<std::uint8_t> bytes{
              0xbd,0x34,0x56,0x8c,0xd8,0x9f,0x5f,0x19,0xc6,0xc3,0xa6,0xe1,0xa6,0x1b,0x92,0x9b,
              0xc8,0x68,0x70,0x94,0x09,0xea,0xad,0x8e,0x67,0x2d,0x85,0xf3,0xc1,0xeb,0x57,0x10};
            const auto append = [&](std::uint64_t n, unsigned width) {
              for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(n >> (i * 8U)));
            };
            append(64, 4); append(20'000'000, 8); append(30'000'000, 8);
            constexpr std::string_view prompt = "official mock AiRequest fixture";
            bytes.insert(bytes.end(), prompt.begin(), prompt.end());
            static constexpr char hx[] = "0123456789abcdef";
            std::string encoded(bytes.size() * 2, '0');
            for (std::size_t i = 0; i < bytes.size(); ++i) {
              encoded[i * 2] = hx[bytes[i] >> 4U]; encoded[i * 2 + 1] = hx[bytes[i] & 15U];
            }
            tx->set_payload(std::move(encoded));
            tx->mutable_verbosedata()->set_transactionid(std::string(64, 'c'));
          }
        }
        break;
      }
      case protowire::KaspadRequest::kGetBlockTemplateRequest: {
        if (!response_wire_.empty()) {
          if (!response.ParseFromArray(response_wire_.data(), static_cast<int>(response_wire_.size())) ||
              !response.has_getblocktemplateresponse() ||
              !response.getblocktemplateresponse().has_block() ||
              !response.getblocktemplateresponse().block().has_header() || network_ != "mainnet")
            return grpc::Status(grpc::StatusCode::DATA_LOSS, "captured template is not a valid mainnet response");
          response.set_id(request.id()); // Correlation ID only; captured template payload is unchanged.
        } else {
          auto *t=response.mutable_getblocktemplateresponse(); t->set_issynced(true);
          auto *h=t->mutable_block()->mutable_header(); h->set_daascore(456); h->set_bits(0x207fffffU);
          h->set_timestamp(1700000000); h->set_nonce(0);
        }
        supplied_template=true; break;
      }
      default: continue;
      }
      if (!stream->Write(response)) break;
      if (close_after_ai_backfill) return grpc::Status::OK;
      if (supplied_template && !hold_) return grpc::Status::OK;
      request.Clear();
    }
    if (chain_status_fixture_) {
      // Deliver out of request order and repeat one response. The client must correlate by
      // request ID and ignore duplicate replies rather than assigning by arrival order.
      for (auto it = deferred_chain_responses.rbegin(); it != deferred_chain_responses.rend(); ++it)
        if (!stream->Write(*it)) break;
      if (!deferred_chain_responses.empty()) (void)stream->Write(deferred_chain_responses.back());
    }
    return grpc::Status::OK;
  }
 private:
  std::vector<std::uint8_t> response_wire_;
  std::string network_;
  bool hold_{};
  bool service_fixture_{};
  bool slow_strikes_{};
  bool ai_block_fixture_{};
  bool chain_status_fixture_{};
};
int main(int argc,char **argv) {
  if(argc<3 || argc>9) {
    std::cerr << "usage: nexus-grpc-mock CERT KEY [MAINNET_CAPTURE] [--hold] [--service-fixture] [--ai-block-fixture] [--chain-status-fixture] [--slow-strikes]\n";
    return 2;
  }
  std::vector<std::uint8_t> response_wire;
  std::string network;
  bool hold=false;
  bool service_fixture=false;
  bool slow_strikes=false;
  bool ai_block_fixture=false;
  bool chain_status_fixture=false;
  try {
    for (int i=3;i<argc;++i) {
      const std::string arg(argv[i]);
      if (arg=="--slow-strikes") { slow_strikes=true; continue; }
      if (arg=="--hold") { hold=true; continue; }
      if (arg=="--service-fixture") { service_fixture=true; continue; }
      if (arg=="--ai-block-fixture") { ai_block_fixture=true; continue; }
      if (arg=="--chain-status-fixture") { chain_status_fixture=true; continue; }
      if (!response_wire.empty()) throw std::runtime_error("only one captured template path is supported");
      const auto records=keryx::nexus::read_capture(arg);
      const auto it=std::find_if(records.begin(),records.end(),[](const auto &r){return r.network=="mainnet";});
      if (it==records.end()) throw std::runtime_error("capture has no mainnet template");
      protowire::KaspadResponse response;
      if (!response.ParseFromArray(it->response_wire.data(),static_cast<int>(it->response_wire.size())) ||
          !response.has_getblocktemplateresponse() || !response.getblocktemplateresponse().issynced())
        throw std::runtime_error("capture has no synced official template response");
      response_wire=it->response_wire;
      network=it->network;
      std::cerr << "MOCK_CAPTURE_REPLAY=READY blake3=" << keryx::nexus::hex(it->digest.data(),it->digest.size()) << "\n";
    }
  } catch(const std::exception &e) { std::cerr << "MOCK_CAPTURE_REPLAY=FAIL " << e.what() << "\n"; return 4; }
  grpc::SslServerCredentialsOptions options;
  grpc::SslServerCredentialsOptions::PemKeyCertPair pair;
  { std::ifstream in(argv[1]); pair.cert_chain.assign(std::istreambuf_iterator<char>(in),{}); }
  { std::ifstream in(argv[2]); pair.private_key.assign(std::istreambuf_iterator<char>(in),{}); }
  options.pem_key_cert_pairs.push_back(pair);
  Mock service(std::move(response_wire),std::move(network),hold,service_fixture,ai_block_fixture,chain_status_fixture,slow_strikes); grpc::ServerBuilder builder; int port=0;
  builder.AddListeningPort("127.0.0.1:0",grpc::SslServerCredentials(options),&port);
  builder.RegisterService(&service); auto server=builder.BuildAndStart();
  if(!server) return 3;
  std::cout<<"PORT="<<port<<std::endl;
  std::string line; std::getline(std::cin,line); server->Shutdown(); return 0;
}
