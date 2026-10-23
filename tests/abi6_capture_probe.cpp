// Offline GPU fixture: never connects to a node and never reads wallet keys.
#include "keryx/nexus/opoi.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
using namespace keryx::nexus::opoi;
namespace {
void u32(std::ostream &out, std::uint32_t n) {
  for (unsigned i=0; i<4; ++i) out.put(static_cast<char>((n >> (8*i)) & 255));
}
void write(const std::filesystem::path &p, const std::vector<std::uint8_t> &bytes) {
  std::ofstream out(p, std::ios::binary);
  out.exceptions(std::ios::badbit | std::ios::failbit);
  out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
}
int main(int argc, char **argv) {
  try {
    if (argc != 5) throw std::invalid_argument("usage: abi6-capture-probe LIB MODEL MANIFEST NEW_PRIVATE_DIRECTORY");
    umask(0077);
    const std::filesystem::path root(argv[4]);
    if (!std::filesystem::create_directory(root)) throw std::runtime_error("Output must be a new private directory");
    std::ifstream manifest_file(argv[3], std::ios::binary);
    std::vector<std::uint8_t> manifest((std::istreambuf_iterator<char>(manifest_file)), {});
    StockLlamaRuntime runtime(argv[1], argv[2]);
    if (runtime.abi_version() != 6) throw std::runtime_error("Fixture requires real ABI6");
    const auto nl = runtime.configure_trace(manifest, 512);
    const auto result = runtime.infer_traced("Reply OK.", nullptr, 4, 32, 0);
    {
      std::ofstream out(root / "capture.bin", std::ios::binary);
      out.exceptions(std::ios::badbit | std::ios::failbit);
      out.write("NXTRACE1", 8); u32(out, result.prompt_positions);
      u32(out, static_cast<std::uint32_t>(result.tokens.size())); u32(out, nl);
      u32(out, static_cast<std::uint32_t>(result.text.size())); out.write(result.text.data(), result.text.size());
      for (auto token : result.tokens) u32(out, static_cast<std::uint32_t>(token));
      for (auto batch : result.ubatch) u32(out, batch);
      for (auto &leaf : result.leaves) out.write(reinterpret_cast<const char *>(leaf.data()), leaf.size());
    }
    // Inspect loaded mappings without touching another GPU.
    std::ifstream maps("/proc/self/maps"); std::string line; bool runtime12=false;
    while (std::getline(maps, line)) {
      if (line.find("libcudart.so.13") != line.npos) throw std::runtime_error("Unexpected CUDA13 runtime loaded");
      runtime12 |= line.find("libcudart.so.12") != line.npos;
    }
    if (!runtime12) throw std::runtime_error("CUDA12 runtime mapping not observed");
    std::cout << "ABI6_CAPTURE_READY positions=" << result.tokens.size() << " leaves=" << nl
              << " cuda_runtime=12\n" << std::flush;
    // The CPU Rust oracle chooses the official draw positions after seeing the
    // captured commitment. Keep this single model resident for those replays.
    std::uint32_t pos, x, y; unsigned index=0;
    while (std::cin >> pos >> x >> y) {
      if (index >= 8) throw std::runtime_error("Replay fixture bound exceeded");
      auto pair = runtime.replay_trace(result, pos, x, y);
      write(root / (std::to_string(index)+".x"), pair.first);
      write(root / (std::to_string(index)+".y"), pair.second);
      ++index;
    }
    if (!std::cin.eof() || index != 4) throw std::runtime_error("Expected exactly four valid Rust-selected replay draws");
    std::cout << "ABI6_REAL_CAPTURE_REPLAY=PASS replays=" << index << " submissions=NONE\n";
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
