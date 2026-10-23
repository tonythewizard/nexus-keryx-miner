#include "keryx/nexus/capture.hpp"
#include "messages.pb.h"
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
  if (argc != 2) { std::cerr << "usage: keryx-nexus-replay CAPTURE\n"; return 2; }
  try {
    const auto records = keryx::nexus::read_capture(argv[1]);
    std::size_t index = 0;
    for (const auto &record : records) {
      protowire::KaspadResponse response;
      if (!response.ParseFromArray(record.response_wire.data(), static_cast<int>(record.response_wire.size())) ||
          !response.has_getblocktemplateresponse())
        throw std::runtime_error("capture record is not an official GetBlockTemplate response");
      const auto &templ = response.getblocktemplateresponse();
      if (!templ.has_block() || !templ.block().has_header())
        throw std::runtime_error("captured template lacks block/header");
      const auto &header = templ.block().header();
      const auto template_wire = templ.SerializeAsString();
      const auto template_digest = keryx::nexus::blake3_digest(
          reinterpret_cast<const std::uint8_t *>(template_wire.data()), template_wire.size());
      ++index;
      std::cout << "REPLAY_TEMPLATE index=" << index << " network=" << record.network
                << " daa=" << header.daascore() << " bits=" << header.bits()
                << " timestamp=" << header.timestamp() << " nonce=" << header.nonce()
                << " synced=" << (templ.issynced() ? "true" : "false")
                << " template_blake3=" << keryx::nexus::hex(template_digest.data(), template_digest.size())
                << " blake3=" << keryx::nexus::hex(record.digest.data(), record.digest.size()) << '\n';
    }
    std::cout << "REPLAY_SUMMARY records=" << records.size() << " submit=DISABLED\n";
    return 0;
  } catch (const std::exception &e) { std::cerr << "REPLAY_ERROR " << e.what() << '\n'; return 1; }
}
