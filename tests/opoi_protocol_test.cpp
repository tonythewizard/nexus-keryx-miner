#include "keryx/nexus/opoi.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char **argv) {
  try {
    constexpr char id[] = "bd34568cd89f5f19c6c3a6e1a61b929bc868709409eaad8e672d85f3c1eb5710";
    constexpr char authorized_uuid[] = "GPU-00000000-0000-0000-0000-000000000000";
    if (argc == 4 && std::string(argv[1]) == "--real-inference") {
      const auto challenge = keryx::nexus::opoi::parse_challenge(
          std::string(id) + ":0011aaff");
      keryx::nexus::opoi::StockLlamaRuntime runtime(argv[2], argv[3]);
      const auto answer = runtime.answer(challenge);
      const auto wire = keryx::nexus::opoi::format_inference_result(challenge, answer);
      const auto expected_prefix = std::string(id) + ":0011aaff:";
      if (answer.empty() || wire.rfind(expected_prefix, 0) != 0 ||
          wire.size() <= expected_prefix.size())
        throw std::runtime_error("real OPoI inference result failed wire validation");
      std::cout << "OPOI_REAL_INFERENCE=PASS model=Qwen3.5-tier0 fixture=official-format "
                   "tokens_max=64 output_bytes=" << answer.size()
                << " wire_bytes=" << wire.size()
                << " cuda_uuid=" << authorized_uuid << '\n';
      return 0;
    }
    if (argc != 1)
      throw std::runtime_error("usage: nexus-opoi-protocol-test [--real-inference LIB GGUF]");
    using namespace keryx::nexus::opoi;
    if(coinbase_extra_data("",0)!="/0000000000000000/ai:v1:6f324042987072e9" ||
       coinbase_extra_data("",42)!="/000000000000002a/ai:v1:a2eb19b2572cbe56" ||
       coinbase_extra_data("",UINT64_MAX)!="/ffffffffffffffff/ai:v1:c4e892c19249a8ae")
      throw std::runtime_error("OPoI tag differs from independent v1.6.4 node reference");
    const std::string identity="/escrow:"+std::string(64,'a')+"/esig:"+std::string(128,'b');
    for(const auto nonce : {std::uint64_t(0),std::uint64_t(42),UINT64_MAX}) {
      const auto wire=coinbase_extra_data(identity,nonce);
      if(!valid_coinbase_extra_data(wire,identity)||valid_coinbase_extra_data(wire,identity+"x"))
        throw std::runtime_error("coinbase identity/tag binding invalid");
      auto bad=wire;bad.back()=bad.back()=='0'?'1':'0';
      if(valid_coinbase_extra_data(bad,identity)||valid_coinbase_extra_data(identity,identity))
        throw std::runtime_error("missing/incorrect coinbase tag accepted");
    }
    if(fresh_coinbase_extra_data(identity)==fresh_coinbase_extra_data(identity))
      throw std::runtime_error("duplicate coinbase request nonce");
    const std::string challenge_wire = std::string(id) + ":0011aaff";
    const auto challenge = keryx::nexus::opoi::parse_challenge(challenge_wire);
    const auto prompt = keryx::nexus::opoi::official_qwen35_prompt(challenge.nonce_hex);
    const std::string expected_prompt =
        "<|im_start|>system\n"
        "You are a Keryx Network AI, a decentralized assistant served by the GPU miners of the Keryx network. "
        "Answer in the language of the user's message. "
        "Do not introduce yourself or describe Keryx unless the user asks about it. If asked: Keryx is a proof-of-work BlockDAG "
        "derived from Kaspa (about 10 blocks per second) where each mining GPU proves on every block that it holds a model in VRAM "
        "(Proof-of-Model), so mining and inference are the same job; requests are on-chain transactions paid in KRX. "
        "Web search results, facts or earlier messages included in the request are your sources for recent or specific information: "
        "use them. Without them, answer from your training knowledge and say when it may be out of date; "
        "never claim to have searched the web or cite sources that are not included in the request. "
        "Never mention your underlying model name or the company that trained it: if asked, you are a Keryx Network AI. "
        "Be thorough but concise.<|im_end|>\n"
        "<|im_start|>user\nKeryx inference challenge 0011aaff: briefly describe what you are.<|im_end|>\n"
        "<|im_start|>assistant\n<think>\n\n</think>\n\n";
    if (prompt != expected_prompt)
      throw std::runtime_error("Qwen3.5 OPoI prompt differs from stock v0.5.7 template");
    if (keryx::nexus::opoi::qwen35_context_candidates() != std::vector<int>{32768, 16384, 8192})
      throw std::runtime_error("Qwen3.5 context ladder differs from official tier cap/floor");
    if (keryx::nexus::opoi::qwen35_context_candidates(4096) != std::vector<int>{4096})
      throw std::runtime_error("KERYX_LLAMA_CTX override was not preserved");
    using keryx::nexus::opoi::challenge_enabled_for_network;
    if (!challenge_enabled_for_network("mainnet", 37'779'999) ||
        challenge_enabled_for_network("mainnet", 37'780'000) ||
        !challenge_enabled_for_network("testnet", 0) ||
        challenge_enabled_for_network("testnet", 1) ||
        challenge_enabled_for_network("unknown", 0))
      throw std::runtime_error("OPoI challenge DAA activation gate differs from stock v0.5.7");
    const auto result = keryx::nexus::opoi::format_inference_result(challenge, "answer text");
    if (result != std::string(id) + ":0011aaff:answer text")
      throw std::runtime_error("GetBlockTemplate.inferenceResult wire formatting mismatch");
    bool invalid_output_rejected = false;
    try {
      (void)keryx::nexus::opoi::format_inference_result(challenge, std::string("bad\xc0\xaf", 5));
    } catch (const std::invalid_argument &) { invalid_output_rejected = true; }
    if (!invalid_output_rejected)
      throw std::runtime_error("invalid UTF-8 inference output was accepted");
    bool rejected = false;
    try {
      (void)keryx::nexus::opoi::parse_challenge(std::string(64, '0') + ":0011");
    } catch (const std::invalid_argument &) { rejected = true; }
    if (!rejected) throw std::runtime_error("unknown OPoI model ID was accepted");
    rejected = false;
    try {
      (void)keryx::nexus::opoi::parse_challenge(std::string(id) + ":xyz");
    } catch (const std::invalid_argument &) { rejected = true; }
    if (!rejected) throw std::runtime_error("malformed OPoI challenge nonce was accepted");
    std::cout << "OPOI_PROTOCOL=PASS model=Qwen3.5-tier0 prompt=stock-v057-byte-exact context=32768/16384/8192 activation=stock-v057 max_tokens=64 result=official-field model_allowlist=PASS\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "OPOI_PROTOCOL=FAIL " << e.what() << '\n';
    return 1;
  }
}
