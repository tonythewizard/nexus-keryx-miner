#include "keryx/nexus/pool_ai_service.hpp"
#include "keryx/nexus/stratum_v4_client.hpp"
#include <fstream>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <iostream>
#include <stdexcept>
#include <zlib.h>
#include <openssl/evp.h>
using namespace keryx::nexus;
void check(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
google::protobuf::Struct parse(const std::string &s) {
  google::protobuf::Struct o;
  check(google::protobuf::util::JsonStringToMessage(s, &o).ok(), "JSON");
  return o;
}
std::vector<std::string> answer_key(const std::string &s) {
  const auto o = parse(s);
  const auto &args = o.fields().at("params").list_value().values();
  check(args.size() == 6, "answer parameter count");
  std::vector<std::string> key{o.fields().at("method").string_value()};
  for (int i = 0; i < 5; ++i)
    key.push_back(args[i].string_value());
  for (auto name : {"trace", "body"})
    key.push_back(args[5].struct_value().fields().at(name).string_value());
  return key;
}
int main(int argc, char **argv) {
  try {
    check(argc == 2, "private test root required");
    std::filesystem::path root = argv[1];
    root = std::filesystem::absolute(root);
    auto model = stratum::parse_hash32_hex(std::string(64, '3'));
    std::string request(64, '1'), response(64, '2'), seed(64, '4');
    const auto wire =
        "{\"method\":\"mining.ai_request\",\"params\":[\"" + request + "\",\"" +
        request + "\",\"" + request + "\",\"" + std::string(64, '3') +
        "\",\"YQBi\",16,\"18446744073709551615\",{\"trace\":{\"tier\":0,"
        "\"prompt_len\":3,\"chain\":true,\"attention\":true}}]}";
    unsigned inferences = 0, openings = 0;
    auto infer = [&](std::string_view prompt, std::uint32_t tokens,
                     const auto &) {
      ++inferences;
      check(prompt == std::string_view("a\0b", 3) && tokens == 16,
            "NUL prompt changed");
      ai::PoolAiService::Capture c;
      c.answer.answer = "OK";
      c.answer.sealed_plaintext = {1, 2, 3};
      c.answer.commitment.fill(0);
      c.answer.commitment[33] = 1;
      c.record = {6, 7, 8};
      return c;
    };
    auto open = [&](auto capture, const auto &, const auto &, const auto &,
                    auto path, auto prev) {
      ++openings;
      check(capture.size() == 3 && capture[0] == 6, "stored capture changed");
      if (!path.empty())
        check(path[0] == 3 && prev[0] == 0xaa, "attention phase changed");
      return std::vector<std::uint8_t>{0xb0, 0};
    };
    std::string first;
    {
      ai::PoolAiService svc(root, model, infer, open);
      first = svc.handle("pool-A", "synthetic", wire);
      check(answer_key(first) ==
                    answer_key(svc.handle("pool-A", "synthetic", wire)) &&
                inferences == 1,
            "duplicate regenerated answer");
    }
    // Simulate a crash after file fsync but before publishing the final name.
    for (auto &dir : std::filesystem::directory_iterator(root))
      for (auto &entry : std::filesystem::directory_iterator(dir.path()))
        if (entry.path().extension() == ".trace") {
          auto pending = entry.path();
          pending += ".new";
          std::filesystem::rename(entry.path(), pending);
        }
    ai::PoolAiService restarted(root, model, infer, open);
    check(answer_key(restarted.handle("pool-A", "synthetic", wire)) ==
                  answer_key(first) &&
              inferences == 1,
          "restart lost immutable answer");
    auto opening = "{\"method\":\"mining.ai_opening\",\"params\":[\"" +
                   request + "\",\"" + response + "\",\"" + seed +
                   "\",[],\"\"]}";
    auto good = parse(restarted.handle("pool-A", "synthetic", opening));
    check(good.fields()
                  .at("params")
                  .list_value()
                  .values(5)
                  .string_value()
                  .empty() &&
              openings == 1,
          "opening failed");
    auto foreign = parse(restarted.handle("pool-B", "synthetic", opening));
    check(!foreign.fields()
                  .at("params")
                  .list_value()
                  .values(5)
                  .string_value()
                  .empty() &&
              openings == 1,
          "cross-pool trace leaked");
    auto changed = opening;
    changed.replace(changed.find(response), 64, std::string(64, '7'));
    auto bad = parse(restarted.handle("pool-A", "synthetic", changed));
    check(!bad.fields()
                  .at("params")
                  .list_value()
                  .values(5)
                  .string_value()
                  .empty() &&
              openings == 1,
          "response rebound");
    auto phase = opening;
    phase.replace(phase.find("[],\"\""), 5, "[3],\"aa\"");
    check(parse(restarted.handle("pool-A", "synthetic", phase))
                  .fields()
                  .at("params")
                  .list_value()
                  .values(5)
                  .string_value()
                  .empty() &&
              openings == 2,
          "attention continuation failed");
    auto invalid = wire;
    invalid.replace(invalid.find("YQBi"), 4, "YQ==");
    bool refused = false;
    try {
      restarted.handle("pool-A", "synthetic", invalid);
    } catch (...) {
      refused = true;
    }
    check(refused && inferences == 1, "conflicting prompt was accepted");
    auto compressed_bound = wire;
    compressed_bound.replace(compressed_bound.find("\"prompt_len\":3"), 14,
                             "\"prompt_len\":2");
    {
      ai::PoolAiService compressed(root / "compressed", model, infer, open);
      check(!compressed.handle("pool-A", "synthetic", compressed_bound).empty(),
            "compressed trace bound confused with decoded byte length");
    }
    check(inferences == 2, "compressed request did not reach inference");
    {
      ai::PoolAiService binding(root / "binding-check", model, infer,
                                [](auto, const auto &, const auto &response,
                                   const auto &, auto, auto) {
                                  if (response[0] == 0x99)
                                    throw std::runtime_error(
                                        "synthetic replay failure");
                                  return std::vector<std::uint8_t>{0xb0, 0};
                                });
      (void)binding.handle("pool-A", "synthetic", wire);
      auto invalid_opening = opening;
      invalid_opening.replace(invalid_opening.find(response), 64,
                              std::string(64, '9'));
      auto failed =
          parse(binding.handle("pool-A", "synthetic", invalid_opening));
      check(!failed.fields()
                 .at("params")
                 .list_value()
                 .values(5)
                 .string_value()
                 .empty(),
            "failed replay returned an opening");
      auto recovered = parse(binding.handle("pool-A", "synthetic", opening));
      check(recovered.fields()
                .at("params")
                .list_value()
                .values(5)
                .string_value()
                .empty(),
            "failed replay poisoned response binding");
    }
    check(inferences == 3, "response binding test did not infer exactly once");
    {
      ai::PoolAiService aliases(root / "alias-check", model, infer, open);
      (void)aliases.handle("stratum+tcp://krx.suprnova.cc:4404", "synthetic",
                           wire);
      const auto result = parse(aliases.handle(
          "stratum+ssl://stratum-eu2.suprnova.cc:4405", "synthetic", opening));
      check(result.fields()
                .at("params")
                .list_value()
                .values(5)
                .string_value()
                .empty(),
            "same-pool alias lost retained trace");
      const auto foreign = parse(aliases.handle(
          "stratum+tcp://s1.keryxpool.com:4401", "synthetic", opening));
      check(!foreign.fields()
                 .at("params")
                 .list_value()
                 .values(5)
                 .string_value()
                 .empty(),
            "different operator inherited trace");
    }
    check(inferences == 4, "same-pool alias regenerated inference");
    // A corrupt durable record is retained and cannot trigger regeneration.
    for (auto &dir : std::filesystem::directory_iterator(root))
      for (auto &entry : std::filesystem::directory_iterator(dir.path()))
        if (entry.path().extension() == ".trace") {
          std::fstream f(entry.path(),
                         std::ios::binary | std::ios::in | std::ios::out);
          f.put('X');
          break;
        }
    refused = false;
    try {
      restarted.handle("pool-A", "synthetic", wire);
    } catch (...) {
      refused = true;
    }
    check(refused && inferences == 4, "corrupt record regenerated response");
    // Test compressed private prompt decompression (\x00KZ\x01 + DEFLATE)
    {
      const std::string text = "Hello compressed prompt world from pool AI!";
      z_stream strm{};
      deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
      strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(text.data()));
      strm.avail_in = static_cast<uInt>(text.size());
      std::string comp = std::string("\x00KZ\x01", 4);
      char zbuf[1024];
      int zret = Z_OK;
      while (zret == Z_OK) {
        strm.next_out = reinterpret_cast<Bytef *>(zbuf);
        strm.avail_out = sizeof(zbuf);
        zret = deflate(&strm, Z_FINISH);
        comp.append(zbuf, sizeof(zbuf) - strm.avail_out);
      }
      deflateEnd(&strm);

      std::string comp_b64(4 * ((comp.size() + 2) / 3), '\0');
      EVP_EncodeBlock(reinterpret_cast<unsigned char *>(comp_b64.data()),
                      reinterpret_cast<const unsigned char *>(comp.data()),
                      static_cast<int>(comp.size()));

      std::string comp_req(64, '9');
      const auto comp_wire =
          "{\"method\":\"mining.ai_request\",\"params\":[\"" + comp_req + "\",\"" +
          comp_req + "\",\"" + comp_req + "\",\"" + std::string(64, '3') +
          "\",\"" + comp_b64 + "\",16,\"18446744073709551615\",{\"trace\":{\"tier\":0,"
          "\"prompt_len\":" + std::to_string(text.size()) + ",\"chain\":true,\"attention\":true}}]}";

      bool comp_infer_called = false;
      auto comp_infer = [&](std::string_view prompt, std::uint32_t tokens, const auto &) {
        comp_infer_called = true;
        check(prompt == text, "compressed prompt did not inflate to original text");
        check(tokens == 16, "tokens mismatch");
        ai::PoolAiService::Capture c;
        c.answer.answer = "OK_COMPRESSED";
        c.answer.sealed_plaintext = {9, 9, 9};
        c.answer.commitment.fill(0);
        c.answer.commitment[33] = 1;
        c.record = {1, 2, 3};
        return c;
      };
      ai::PoolAiService comp_svc(root / "comp", model, comp_infer, open);
      auto comp_res = comp_svc.handle("pool-A", "synthetic", comp_wire);
      check(comp_infer_called, "compressed prompt infer was not called");
      check(!comp_res.empty(), "empty compressed response");
    }
    std::cout << "POOL_AI_SERVICE=PASS nul_exact=true durable_restart=true "
                 "interrupted_publication_recovered=true "
                 "duplicate_no_regen=true response_immutable=true "
                 "pool_isolation=true attention_phase=true "
                 "corruption_fail_closed=true mainnet_transactions=0\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "POOL_AI_SERVICE=FAIL " << e.what() << '\n';
    return 1;
  }
}
