#include "keryx/nexus/consensus/pom_v4.hpp"
#include "keryx/nexus/h15_audit_client.hpp"
#include <fstream>
#include <iostream>
#ifdef NEXUS_TEST_HAS_CUDA
#include "keryx/nexus/consensus/cuda_resident.hpp"
#include "keryx/nexus/developer_config.hpp"
#endif
#include <stdexcept>
using namespace keryx::nexus;
int main(int argc, char **argv) {
  try {
    if (argc != 7)
      throw std::runtime_error("usage: h15-test wrapper model tree "
                               "isolated-state endpoint private-fixture");
    opoi::StockLlamaRuntime runtime(argv[1], argv[2]);
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
#ifdef NEXUS_TEST_HAS_CUDA
    std::unique_ptr<consensus::CudaResidentSearch> resident;
    unsigned cycles = 0;
    auto mining_cycle = [&] {
      runtime.use_task_context();
      resident = std::make_unique<consensus::CudaResidentSearch>(
          primary_gpu_uuid(), gguf, runtime.resident_tensors());
      consensus::Hash v4_pph{};v4_pph.fill(static_cast<uint8_t>(42+cycles));
      const auto v4_nonce=12340+cycles;
      const auto v4_daa=consensus::pom_v5_activation_daa-1;
      const auto v4_seed=consensus::block_seed(v4_pph,1700000123,v4_nonce,v4_daa);
      const auto v4_cpu=consensus::prove_v4_from_model(v4_seed,0,gguf.chunk_count()/32,
          [&](auto off){std::array<uint8_t,consensus::tile_bytes> tile{};gguf.read_tile_into(off,tile);return tile;},
          [&](auto off){return merkle.tile_path(off);});
      auto v4_target=consensus::pom_pow_value_h3(consensus::final_state(v4_cpu.walk),v4_pph);
      if(resident->run(v4_pph,1700000123,v4_daa,v4_target,v4_nonce,1).winner!=v4_nonce)
        throw std::runtime_error("v4 pipeline exact target rejected");
      for(auto &byte:v4_target){if(byte--!=0)break;}
      if(resident->run(v4_pph,1700000123,v4_daa,v4_target,v4_nonce,1).winner)
        throw std::runtime_error("v4 pipeline below target accepted");
      consensus::Hash zero_target{};
      const auto v4_batch=resident->run(v4_pph,1700000123,v4_daa,zero_target,v4_nonce,4608);
      if(v4_batch.winner)throw std::runtime_error("unexpected zero-target winner");
      std::cout<<"V4_RUNTIME_PIPELINE=PASS cycle="<<cycles<<" exact_target=true below_target=true batch_ms="<<v4_batch.kernel_ms<<std::endl;
      const auto root =
          consensus::official_tier_anchor(consensus::pom_v5_activation_daa, 0)
              .root;
      const auto pad = consensus::official_v5_pad(0, model_id, root);
      if (!resident->prepare_v5(pad))
        throw std::runtime_error("native handoff pad preparation cancelled");
      consensus::Hash pph{}, target{};
      pph.fill(42);
      target.fill(255);
      const auto nonce = 12340 + cycles;
      constexpr std::uint64_t timestamp = 1700000123;
      const auto seed = consensus::block_seed(pph, timestamp, nonce,
                                              consensus::pom_v5_activation_daa);
      const auto cpu = consensus::prove_v5_from_model(
          seed, 0, gguf.chunk_count() / 32, pad,
          [&](auto off) {
            std::array<std::uint8_t, consensus::tile_bytes> tile{};
            gguf.read_tile_into(off, tile);
            return tile;
          },
          [&](auto off) { return merkle.tile_path(off); });
      const auto gpu = resident->diagnose_v5(
          pph, timestamp, consensus::pom_v5_activation_daa, nonce);
      if (cpu.walk.states != gpu.states || cpu.walk.offsets != gpu.offsets)
        throw std::runtime_error("native handoff PoM v5 CPU/GPU mismatch");
      if (resident
              ->run(pph, timestamp, consensus::pom_v5_activation_daa, target,
                    nonce, 1)
              .winner != nonce)
        throw std::runtime_error("native handoff winner mismatch");
      const auto wire = consensus::borsh_v5_container(
          cpu.proof, consensus::final_state(gpu),
          consensus::pom_pow_value_h3(consensus::final_state(gpu), pph));
      std::ofstream out(std::filesystem::path(argv[4]).parent_path() /
                            ("real-handoff-" + std::to_string(cycles) + ".bin"),
                        std::ios::binary);
      auto put = [&](std::uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i)
          out.put(static_cast<char>(value >> (8 * i)));
      };
      auto hash = [&](const auto &value) {
        out.write(reinterpret_cast<const char *>(value.data()), value.size());
      };
      out.write("NXV5", 4);
      put(0, 1);
      hash(model_id);
      hash(root);
      put(gguf.chunk_count(), 8);
      put(pad.tiles, 8);
      put(seed, 8);
      hash(pph);
      hash(target);
      put(timestamp, 8);
      put(nonce, 8);
      put(consensus::pom_v5_activation_daa, 8);
      put(257, 4);
      put(256, 4);
      for (const auto &state : gpu.states)
        hash(state);
      for (auto offset : gpu.offsets)
        put(offset, 8);
      put(wire.size(), 4);
      hash(wire);
      out.flush();
      if (!out)
        throw std::runtime_error("native handoff artifact write failed");
      ++cycles;
      std::cout << "H15_REAL_INFERENCE_HANDOFF_POM=PASS cycle=" << cycles
                << " states=257 offsets=256\n"
                << std::flush;
    };
    mining_cycle();
#endif
    client.set_gpu_owner([&](const std::function<void()> &work) {
#ifdef NEXUS_TEST_HAS_CUDA
      resident.reset();
#endif
      work();
    });
    for (const auto &prompt :
         {std::string("Reply with one word."),
          std::string(2400, 'a') + " Reply with one word."}) {
#ifdef NEXUS_TEST_HAS_CUDA
      resident.reset();
#endif
      const auto traced = client.infer(prompt, 32, request);
      if (traced.answer.empty() || traced.sealed_plaintext.empty())
        throw std::runtime_error("missing traced answer");
      const auto stats = client.verify_capture(2);
      std::cout << "H15_NATIVE_TRACE=PASS pieces=" << stats[0]
                << " child_phases=" << stats[1] << " positions="
                << (std::uint32_t(traced.commitment[36]) |
                    (std::uint32_t(traced.commitment[37]) << 8) |
                    (std::uint32_t(traced.commitment[38]) << 16) |
                    (std::uint32_t(traced.commitment[39]) << 24))
                << " submissions=NONE\n"
                << std::flush;
    }
#ifdef NEXUS_TEST_HAS_CUDA
    mining_cycle();
    resident.reset();
#endif
    const auto lifecycle = client.verify_lifecycle();
    std::cout << "H15_NATIVE_LIFECYCLE=PASS pieces=" << lifecycle[0]
              << " child_phases=" << lifecycle[1] << " replays=" << lifecycle[2]
              << " restart=" << lifecycle[3]
              << " reorg=PASS cached_reinclusion=PASS transactions=NOT_SENT\n";
#ifdef NEXUS_TEST_HAS_CUDA
    mining_cycle();
    resident.reset();
#endif

    // Real traced inference, signing and durable native commit; submission is
    // an in-memory effect and never calls the node's mutation RPCs.
    {
      std::ifstream input(argv[6], std::ios::binary);
      auto read = [&](void *out, std::size_t count) {
        input.read(static_cast<char *>(out),
                   static_cast<std::streamsize>(count));
        if (!input)
          throw std::runtime_error("private fixture truncated");
      };
      std::array<std::uint8_t, 4> length{};
      read(length.data(), 4);
      std::uint32_t size = 0;
      for (unsigned i = 0; i < 4; ++i)
        size |= std::uint32_t(length[i]) << (8 * i);
      if (size > 2U << 20)
        throw std::runtime_error("private fixture bounds");
      std::vector<std::uint8_t> payload(size);
      read(payload.data(), size);
      std::uint8_t secret_len{};
      read(&secret_len, 1);
      if (secret_len != 32 && secret_len != 33)
        throw std::runtime_error("private fixture key schedule");
      std::vector<std::uint8_t> requester_secret(secret_len);
      read(requester_secret.data(), secret_len);
      ai::AiResponseConfig config;
      read(config.escrow_pubkey.data(), 32);
      read(config.delegation_signature.data(), 64);
      if (input.peek() != EOF)
        throw std::runtime_error("private fixture trailing bytes");
      config.escrow_secret.fill(0x22);
      config.model_id = model_id;
      config.native_h15 = true;
      config.payout_address =
          "keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr";
      config.service_identity =
          *ai::service_identity_from_payout(config.payout_address);
      config.tier = 0;
      config.private_inline_activation_daa =
          ai::mainnet_private_inference_gate_daa;
      config.signed_response_activation_daa = 0;
      const auto state =
          std::filesystem::path(argv[4]).parent_path() / "service-h15";
      config.journal_path = state.parent_path() / "real-service.journal";
      protowire::RpcTransaction tx;
      tx.set_subnetworkid("0300000000000000000000000000000000000000");
      tx.set_payload(ai::hex_encode(payload.data(), payload.size()));
      constexpr std::uint64_t daa = 140115100;
      const auto parsed = ai::parse_request(tx, daa);
      if (!parsed || !parsed->is_private)
        throw std::runtime_error("private fixture parse");
      auto native_service = std::make_unique<ai::H15AuditClient>(
          runtime, gguf, merkle, state, config.escrow_secret, argv[5],
          model_id);
      native_service->set_gpu_owner([](const auto &work) { work(); });
      ai::AiResponseService response_service(config);
      response_service.recover();
      ai::AiResponseEffects effects;
      unsigned generations = 0, submissions = 0;
      std::vector<std::uint8_t> packed;
      effects.infer = [](auto, auto) -> std::string {
        throw std::runtime_error("untraced inference invoked");
      };
      effects.infer_traced = [&](auto prompt, auto tokens, const auto &hash) {
        ++generations;
        auto answer = native_service->infer(prompt, tokens, hash);
        native_service->verify_capture(1);
        packed = answer.sealed_plaintext;
        return answer;
      };
      effects.persist_trace = [&](auto id, auto body, auto at, auto expiry) {
        native_service->commit(id, body, at, expiry);
      };
      effects.recover_traced = [&](const auto &hash) {
        return native_service->response_for_request(hash);
      };
      effects.publish_and_readback = [](auto) -> std::array<std::uint8_t, 34> {
        throw std::runtime_error("private inline response tried IPFS");
      };
      effects.current_daa = [] { return daa; };
      effects.reconcile = [](auto) { return false; };
      effects.submit = [&](const auto &response, std::string_view id) {
        if (native_service->response_for_request(parsed->request_hash) != id)
          throw std::runtime_error(
              "real response reached effect before native persistence");
        std::vector<std::uint8_t> wire(response.payload().size() / 2);
        auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        for (std::size_t i = 0; i < wire.size(); ++i)
          wire[i] = std::uint8_t((digit(response.payload()[2 * i]) << 4) |
                                 digit(response.payload()[2 * i + 1]));
        std::vector<std::uint8_t> opened;
        if (!ai::stock_verify_response(wire, parsed->request_hash,
                                       config.escrow_pubkey, requester_secret,
                                       &opened) ||
            opened != packed)
          throw std::runtime_error(
              "real private token response signature/sealing mismatch");
        native_service->response_intent(id);
        native_service->response_result(id, false);
        ++submissions;
        return ai::AiResponseEffects::SubmitResult{
            ai::AiResponseEffects::SubmitResult::State::Unknown,
            "simulated lost acknowledgement; no network effect"};
      };
      if (response_service.serve(tx, daa, {}, effects, daa + 500, daa) !=
              ai::ServeResult::WaitingReconciliation ||
          generations != 1 || submissions != 1)
        throw std::runtime_error("real private native service lifecycle");
      const auto saved =
          native_service->response_for_request(parsed->request_hash);
      native_service.reset();
      native_service = std::make_unique<ai::H15AuditClient>(
          runtime, gguf, merkle, state, config.escrow_secret, argv[5],
          model_id);
      native_service->poll();
      if (native_service->response_for_request(parsed->request_hash) != saved)
        throw std::runtime_error("real private service restart lost signed ID");
      ai::AiResponseService recovered(config);
      recovered.recover();
      effects.infer_traced = [](auto, auto,
                                const auto &) -> ai::AiTracedAnswer {
        throw std::runtime_error("restart regenerated answer");
      };
      effects.submit = [](const auto &,
                          auto) -> ai::AiResponseEffects::SubmitResult {
        throw std::runtime_error("restart bypassed durable retry scheduler");
      };
      if (recovered.serve(tx, daa, {}, effects, daa + 500, daa) !=
          ai::ServeResult::WaitingReconciliation)
        throw std::runtime_error("real private service restart recovery");
      std::cout
          << "H15_REAL_PRIVATE_SERVICE=PASS GPU=REAL signature=PASS "
             "token_plaintext=PASS durable_commit=PASS restart=NO_REGENERATION "
             "transport=IN_MEMORY transactions=NOT_SENT\n"
          << std::flush;
    }
#ifdef NEXUS_TEST_HAS_CUDA
    mining_cycle();
    resident.reset();
#endif
    std::cout << "H15_NATIVE_TEST=PASS gpu=REAL transactions=NOT_SENT\n";
  } catch (const std::exception &e) {
    std::cerr << "H15_NATIVE_TEST=FAIL " << e.what() << '\n';
    return 1;
  }
}
