#include "keryx/nexus/h15_audit_client.hpp"
#include "keryx/nexus/pool_ai_service.hpp"
#include "keryx/nexus/stratum_v4_client.hpp"
#include <algorithm>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <iostream>
#include <fstream>
#include <cstdlib>
#ifdef NEXUS_TEST_HAS_CUDA
#include <cuda.h>
#endif
#include <stdexcept>

using namespace keryx::nexus;
int main(int argc, char **argv) {
  try {
    if (argc != 6 && argc != 7)
      throw std::runtime_error(
          "usage: nul-test runtime model tree isolated-state mock-endpoint");
    opoi::StockLlamaRuntime runtime(argv[1], argv[2]);
    if (!runtime.supports_length_aware_tracing())
      throw std::runtime_error("runtime lacks sized traced entry point");
#ifdef NEXUS_TEST_HAS_CUDA
    struct Pressure { CUdeviceptr ptr{}; ~Pressure(){if(ptr)cuMemFree(ptr);} } pressure;
    if(const auto raw=std::getenv("NEXUS_TEST_VRAM_PRESSURE_MIB")) {
      runtime.use_task_context();
      const auto mib=std::stoul(raw);
      if(mib>1024 || cuMemAlloc(&pressure.ptr,mib<<20)!=CUDA_SUCCESS)
        throw std::runtime_error("test VRAM pressure allocation failed");
    }
#endif
    model::ReadOnlyGguf gguf(argv[2]);
    model::SparseMerkleIndex merkle(gguf, argv[3]);
    constexpr std::array<std::uint8_t, 32> model_id{
        0xbd, 0x34, 0x56, 0x8c, 0xd8, 0x9f, 0x5f, 0x19, 0xc6, 0xc3, 0xa6,
        0xe1, 0xa6, 0x1b, 0x92, 0x9b, 0xc8, 0x68, 0x70, 0x94, 0x09, 0xea,
        0xad, 0x8e, 0x67, 0x2d, 0x85, 0xf3, 0xc1, 0xeb, 0x57, 0x10};
    std::array<std::uint8_t, 32> secret{}, request{};
    secret.fill(1);
    request.fill(2);
    ai::H15AuditClient client(runtime, gguf, merkle, argv[4], secret, argv[5],
                              model_id, true);
    client.set_gpu_owner([](const auto &work) { work(); });
    constexpr char raw[] = "Say OK.\0Then say SAFE.\0";
    std::string input(raw, sizeof(raw) - 1);
    if(argc==7) {
      std::ifstream file(argv[6],std::ios::binary);
      if(!file) throw std::runtime_error("prompt fixture missing");
      input.assign(std::istreambuf_iterator<char>(file),{});
    }
    const std::string_view prompt(input);
    if (argc==6 && std::count(prompt.begin(), prompt.end(), '\0') != 2)
      throw std::runtime_error("NUL fixture invalid");
    const auto traced = client.infer(prompt, 16, request);
    if (traced.answer.empty() || traced.sealed_plaintext.empty())
      throw std::runtime_error("NUL inference returned no capture");
    const auto verified = client.verify_capture(2);
    if (verified[0] == 0)
      throw std::runtime_error("NUL trace had no independently checked points");
    if(argc==7) {
      const auto bytes=client.export_pool_capture();
      if(bytes.size()<=64U<<20) throw std::runtime_error("long capture did not exceed legacy storage cap");
      std::array<std::uint8_t,32> response{},seed{};response.fill(3);seed.fill(7);
      const auto opening=client.open_pool_capture(bytes,request,response,seed,{},{});
      if(ai::H15AuditClient::unbundle(opening).size()!=4)
        throw std::runtime_error("large persisted opening failed");
      std::cout<<"H15_LARGE_CAPTURE=PASS bytes="<<bytes.size()<<" verified_points="<<verified[0]<<" persisted_opening_pieces=4"<<std::endl;
      return 0;
    }
    unsigned pool_inferences = 0;
    const auto infer = [&](std::string_view input, std::uint32_t tokens,
                           const auto &hash) {
      if (input != prompt)
        throw std::runtime_error("pool prompt bytes changed");
      ++pool_inferences;
      ai::PoolAiService::Capture result;
      result.answer = client.infer(input, tokens, hash);
      result.record = client.export_pool_capture();
      return result;
    };
    const auto open = [&](auto bytes, const auto &req, const auto &resp,
                          const auto &seed, auto path, auto previous) {
      return client.open_pool_capture(bytes, req, resp, seed, path, previous);
    };
    const auto request_hex = stratum::hex_encode_bytes(request);
    const auto request_wire =
        "{\"method\":\"mining.ai_request\",\"params\":[\"" + request_hex +
        "\",\"" + request_hex + "\",\"" + request_hex +
        "\","
        "\"bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710\","
        "\"U2F5IE9LLgBUaGVuIHNheSBTQUZFLgA=\",16,\"1\",{\"trace\":{\"tier\":0,"
        "\"prompt_len\":23,\"chain\":true,\"attention\":true}}]}";
    std::string first;
    const auto records = std::filesystem::path(argv[4]) / "pool-records";
    {
      ai::PoolAiService pool(records, model_id, infer, open);
      first = pool.handle("stratum+tcp://krx.suprnova.cc:4404",
                          "synthetic-worker", request_wire);
    }
    ai::PoolAiService restored(records, model_id, infer, open);
    const auto again = restored.handle("stratum+ssl://krx.suprnova.cc:4405",
                                       "synthetic-worker", request_wire);
    if (pool_inferences != 1)
      throw std::runtime_error("pool restart regenerated inference");
    const auto opening = "{\"method\":\"mining.ai_opening\",\"params\":[\"" +
                         request_hex + "\",\"" + std::string(64, '3') +
                         "\",\"" + std::string(64, '7') + "\",[],\"\"]}";
    google::protobuf::Struct result;
    if (!google::protobuf::util::JsonStringToMessage(
             restored.handle("stratum+ssl://stratum-eu2.suprnova.cc:4405",
                             "synthetic-worker", opening),
             &result)
             .ok())
      throw std::runtime_error("pool opening result JSON");
    const auto &params = result.fields().at("params").list_value().values();
    if (params.size() != 6 || !params[5].string_value().empty())
      throw std::runtime_error("pool durable opening failed");
    const auto body = stratum::hex_decode_bytes(params[4].string_value());
    const auto pieces = ai::H15AuditClient::unbundle(body);
    if (pieces.size() != 4)
      throw std::runtime_error("pool opening must contain "
                               "chain/attention/recurrent/residual pieces");
    std::cout
        << "POOL_AI_NATIVE=PASS nul_bytes_exact=true durable_capture=true "
           "restart_no_regeneration=true same_operator_alias=true "
           "opening_verified_pieces="
        << pieces.size()
        << " pool_signing_required=true mainnet_transactions=0\n";
    std::cout << "H15_NUL_NATIVE=PASS prompt_bytes=" << prompt.size()
              << " embedded_nuls=2 runtime_sized=true verified_points="
              << verified[0] << " attention_child_phases=" << verified[1]
              << " seeds=2 mainnet_transactions=0\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "H15_NUL_NATIVE=FAIL " << e.what() << '\n';
    return 1;
  }
}
