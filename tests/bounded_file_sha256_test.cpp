#include "keryx/nexus/bounded_file_sha256.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;
using namespace keryx::nexus;

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
static std::string reference(const std::vector<unsigned char> &data) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned length{};
  require(EVP_Digest(data.data(), data.size(), digest.data(), &length,
                     EVP_sha256(), nullptr) == 1 && length == 32, "reference digest");
  std::string result;
  for (unsigned i = 0; i < length; ++i) {
    result += "0123456789abcdef"[digest[i] >> 4];
    result += "0123456789abcdef"[digest[i] & 15];
  }
  return result;
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "persistent fixture directory required");
    const fs::path root = fs::absolute(argv[1]);
    require(!root.string().starts_with("/tmp"), "persistent fixture directory required");
    fs::create_directories(root);
    unsigned cases = 0, direct = 0;
    for (const std::size_t size : {0UL, 3UL, 4095UL, 4096UL, 4097UL,
                                  1048576UL + 123, 33554432UL + 17}) {
      std::vector<unsigned char> bytes(size);
      for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<unsigned char>(i * 17 + 3);
      const auto file = root / ("vector-" + std::to_string(size));
      {
        std::ofstream output(file, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        require(bool(output), "fixture write");
      }
      const auto expected = reference(bytes);
      FileHashIoStats regular, preferred;
      require(bounded_file_sha256(file.string(), &regular, false) == expected,
              "buffered SHA-256 differs from independent digest");
      require(!regular.direct_io && regular.bytes == size && regular.advised_bytes == size,
              "buffered accounting lost bytes/cache advice");
      require(bounded_file_sha256(file.string(), &preferred) == expected,
              "preferred SHA-256 differs from independent digest");
      require(preferred.bytes == size, "preferred byte accounting");
      if (preferred.direct_io) {
        ++direct;
        require(preferred.advised_bytes == 0, "direct I/O unexpectedly populated buffered path");
      }
      ++cases;
    }
    const auto abc = root / "abc";
    { std::ofstream f(abc); f << "abc"; }
    require(bounded_file_sha256(abc.string()) ==
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "standard SHA-256 abc vector");
    const auto fifo = root / "not-a-model.fifo";
    require(::mkfifo(fifo.c_str(), 0600) == 0 || errno == EEXIST, "FIFO fixture creation");
    for (const auto &invalid : {root / "does-not-exist", root, fifo}) {
      bool rejected = false;
      try { (void)bounded_file_sha256(invalid.string()); }
      catch (const std::runtime_error &) { rejected = true; }
      require(rejected, "invalid SHA-256 input accepted");
    }
    std::cout << "BOUNDED_SHA256=PASS sizes=" << cases << " direct_cases=" << direct
              << " buffered_fallback=PASS unaligned_tail=PASS standard_vector=PASS"
                 " invalid_input=REJECTED\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "BOUNDED_SHA256=FAIL " << error.what() << '\n';
    return 1;
  }
}
