#include "keryx/nexus/capture.hpp"

#include <blake3.h>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace keryx::nexus {
namespace {
constexpr std::array<char, 8> magic{'K','N','X','C','A','P','1','\n'};
constexpr std::uint32_t max_record_size = 128U * 1024U * 1024U;

void write_u32(std::ostream &out, std::uint32_t value) {
  const std::array<char, 4> b{static_cast<char>(value), static_cast<char>(value >> 8U),
                              static_cast<char>(value >> 16U), static_cast<char>(value >> 24U)};
  out.write(b.data(), static_cast<std::streamsize>(b.size()));
}
std::uint32_t read_u32(std::istream &in) {
  std::array<unsigned char, 4> b{};
  in.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size()));
  if (in.gcount() != static_cast<std::streamsize>(b.size()))
    throw std::runtime_error("truncated capture record length");
  return static_cast<std::uint32_t>(b[0]) |
         (static_cast<std::uint32_t>(b[1]) << 8U) |
         (static_cast<std::uint32_t>(b[2]) << 16U) |
         (static_cast<std::uint32_t>(b[3]) << 24U);
}
}

std::array<std::uint8_t, 32> blake3_digest(const std::uint8_t *data, std::size_t size) {
  std::array<std::uint8_t, 32> digest{};
  blake3_hasher hasher;
  blake3_hasher_init(&hasher);
  blake3_hasher_update(&hasher, data, size);
  blake3_hasher_finalize(&hasher, digest.data(), digest.size());
  return digest;
}

std::string hex(const std::uint8_t *data, std::size_t size) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out(size * 2, '0');
  for (std::size_t i = 0; i < size; ++i) {
    out[2 * i] = digits[data[i] >> 4U];
    out[2 * i + 1] = digits[data[i] & 0x0fU];
  }
  return out;
}

CaptureWriter::CaptureWriter(const std::string &path) : path_(path) {
  std::ifstream existing(path_, std::ios::binary);
  if (existing) {
    std::array<char, magic.size()> header{};
    existing.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (existing.gcount() != static_cast<std::streamsize>(header.size()) || header != magic)
      throw std::runtime_error("capture exists with an invalid NEXUS header");
  } else {
    std::ofstream create(path_, std::ios::binary | std::ios::trunc);
    if (!create) throw std::runtime_error("cannot create capture file: " + path_);
    create.write(magic.data(), static_cast<std::streamsize>(magic.size()));
    create.flush();
    if (!create) throw std::runtime_error("cannot initialize capture file");
  }
}

void CaptureWriter::append(const std::string &network,
                           const std::vector<std::uint8_t> &response_wire) {
  if (network.size() > std::numeric_limits<std::uint16_t>::max())
    throw std::runtime_error("network string exceeds capture format limit");
  if (response_wire.empty()) throw std::runtime_error("empty protobuf response cannot be captured");
  const std::uint64_t body_size64 = 2ULL + network.size() + response_wire.size();
  if (body_size64 > max_record_size) throw std::runtime_error("template capture exceeds record limit");
  const auto body_size = static_cast<std::uint32_t>(body_size64);
  std::vector<std::uint8_t> body;
  body.reserve(body_size);
  body.push_back(static_cast<std::uint8_t>(network.size()));
  body.push_back(static_cast<std::uint8_t>(network.size() >> 8U));
  body.insert(body.end(), network.begin(), network.end());
  body.insert(body.end(), response_wire.begin(), response_wire.end());
  const auto digest = blake3_digest(body.data(), body.size());

  std::ofstream out(path_, std::ios::binary | std::ios::app);
  if (!out) throw std::runtime_error("cannot append capture file: " + path_);
  write_u32(out, body_size);
  out.write(reinterpret_cast<const char *>(body.data()), static_cast<std::streamsize>(body.size()));
  out.write(reinterpret_cast<const char *>(digest.data()), static_cast<std::streamsize>(digest.size()));
  out.flush();
  if (!out) throw std::runtime_error("capture write failed");
}

std::vector<CaptureRecord> read_capture(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open capture file: " + path);
  std::array<char, magic.size()> header{};
  in.read(header.data(), static_cast<std::streamsize>(header.size()));
  if (in.gcount() != static_cast<std::streamsize>(header.size()) || header != magic)
    throw std::runtime_error("invalid NEXUS capture magic/version");
  std::vector<CaptureRecord> records;
  while (in.peek() != std::char_traits<char>::eof()) {
    const auto length = read_u32(in);
    if (length < 2 || length > max_record_size)
      throw std::runtime_error("capture record length out of range");
    std::vector<std::uint8_t> body(length);
    in.read(reinterpret_cast<char *>(body.data()), static_cast<std::streamsize>(body.size()));
    if (in.gcount() != static_cast<std::streamsize>(body.size()))
      throw std::runtime_error("truncated capture record body");
    std::array<std::uint8_t, 32> expected{};
    in.read(reinterpret_cast<char *>(expected.data()), static_cast<std::streamsize>(expected.size()));
    if (in.gcount() != static_cast<std::streamsize>(expected.size()))
      throw std::runtime_error("truncated capture BLAKE3 digest");
    const auto actual = blake3_digest(body.data(), body.size());
    if (actual != expected) throw std::runtime_error("capture BLAKE3 checksum mismatch");
    const auto network_size = static_cast<std::size_t>(body[0]) |
                              (static_cast<std::size_t>(body[1]) << 8U);
    if (network_size > body.size() - 2) throw std::runtime_error("invalid capture network field size");
    CaptureRecord r;
    r.network.assign(reinterpret_cast<const char *>(body.data() + 2), network_size);
    r.response_wire.assign(body.begin() + static_cast<std::ptrdiff_t>(2 + network_size), body.end());
    r.digest = expected;
    if (r.response_wire.empty()) throw std::runtime_error("capture contains an empty response");
    records.push_back(std::move(r));
  }
  if (records.empty()) throw std::runtime_error("capture contains no template records");
  return records;
}

} // namespace keryx::nexus
