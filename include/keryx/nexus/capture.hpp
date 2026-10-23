#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace keryx::nexus {

struct CaptureRecord {
  std::string network;
  std::vector<std::uint8_t> response_wire;
  std::array<std::uint8_t, 32> digest{};
};

class CaptureWriter {
public:
  explicit CaptureWriter(const std::string &path);
  void append(const std::string &network, const std::vector<std::uint8_t> &response_wire);
private:
  std::string path_;
};

std::vector<CaptureRecord> read_capture(const std::string &path);
std::array<std::uint8_t, 32> blake3_digest(const std::uint8_t *data, std::size_t size);
std::string hex(const std::uint8_t *data, std::size_t size);

} // namespace keryx::nexus
