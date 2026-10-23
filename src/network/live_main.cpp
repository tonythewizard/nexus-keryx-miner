#include "keryx/nexus/grpc_client.hpp"
#include "keryx/nexus/grpc_runtime.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
std::atomic_bool stop_requested{false};
extern "C" void on_signal(int) { stop_requested.store(true, std::memory_order_relaxed); }
std::uint64_t parse_u64(const char *text, const char *option) {
  std::uint64_t value{};
  const std::string input(text);
  const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
  if (error != std::errc{} || end != input.data() + input.size())
    throw std::runtime_error(std::string("invalid value for ") + option);
  return value;
}
}

int main(int argc, char **argv) {
  keryx::nexus::network::GrpcRuntime grpc_runtime;
  if (argc < 10 || std::string(argv[1]) != "--dry-run" ||
      std::string(argv[2]) != "--endpoint" || std::string(argv[4]) != "--pay-address" ||
      std::string(argv[6]) != "--extra-data" || std::string(argv[8]) != "--capture") {
    std::cerr << "usage: keryx-nexus-live --dry-run --endpoint grpc(s)://host:port "
                 "--pay-address ADDRESS --extra-data TEXT --capture FILE "
                 "[--max-templates N] [--reconnect-after-seconds N]\n"
                 "Block submission is not implemented.\n";
    return 2;
  }
  try {
    std::uint64_t max_templates = 0;
    unsigned reconnect_seconds = 0;
    for (int i = 10; i < argc; i += 2) {
      if (i + 1 >= argc) throw std::runtime_error("option requires a value");
      const std::string option(argv[i]);
      if (option == "--max-templates") max_templates = parse_u64(argv[i + 1], argv[i]);
      else if (option == "--reconnect-after-seconds") {
        const auto value = parse_u64(argv[i + 1], argv[i]);
        if (value > std::numeric_limits<unsigned>::max()) throw std::runtime_error("reconnect interval too large");
        reconnect_seconds = static_cast<unsigned>(value);
      } else throw std::runtime_error("unsupported option: " + option);
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    return keryx::nexus::run_read_only_client(argv[3], argv[5], argv[7], argv[9],
                                              max_templates, reconnect_seconds, stop_requested);
  } catch (const std::exception &e) {
    std::cerr << "NEXUS_ERROR " << e.what() << " submit=DISABLED\n";
    return 1;
  }
}
