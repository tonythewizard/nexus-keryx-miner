#include "keryx/nexus/trace_capture.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace keryx::nexus::opoi;
namespace {
int mode = 0, calls = 0;
int configure(void *, const void *, std::size_t, int) { ++calls; return mode == 1 ? -5 : 0; }
int count(void *) { return mode == 2 ? -1 : 3; }
int generate(void *, const char *, const std::int32_t *, int, int, const char *, int,
             char *out, int, std::int32_t *tokens, int, int *prompt, int *fed, int, int) {
  ++calls;
  std::memcpy(out, "OK", 2); tokens[0] = 1; tokens[1] = mode == 8 ? -1 : 2;
  *prompt = mode == 5 ? -1 : 1; *fed = mode == 6 ? 500 : 2;
  return mode == 3 ? -2 : mode == 4 ? 65536 : 2;
}
int read(void *, std::uint8_t *, std::size_t, std::uint32_t *ubatch, std::size_t, std::uint32_t *pos) {
  ++calls; ubatch[0] = 1; ubatch[1] = mode == 9 ? 0 : 1; *pos = mode == 7 ? 1 : 2;
  return mode == 10 ? -6 : 0;
}
int replay(void *, const std::int32_t *, int, int, std::uint32_t, std::uint32_t, std::uint32_t,
           std::uint8_t *x, std::size_t, std::uint8_t *y, std::size_t, std::uint32_t *nx, std::uint32_t *ny) {
  ++calls; x[0] = 42; y[0] = 43; *nx = mode == 11 ? (4U << 20) + 1 : 1; *ny = 1;
  return mode == 12 ? -7 : 0;
}
void check(bool v) { if (!v) throw std::runtime_error("test assertion failed"); }
int checks = 0;
template<class F> void rejects(F f) {
  bool threw = false; try { f(); } catch (const std::exception &) { threw = true; }
  check(threw); ++checks;
}
}
int main() {
  try {
    detail::TraceApi api{configure, count, generate, read, replay, nullptr};
    std::array<std::uint8_t,16> manifest{};
    std::memcpy(manifest.data(), "KXMANIF1", 8); manifest[8] = 1; manifest[12] = 1;
    detail::TraceCapture capture(api, &mode, 8192);
    rejects([&] { detail::TraceCapture bad({}, &mode, 8192); });
    rejects([&] { capture.generate("hello", nullptr, 1, 0, 0); });
    rejects([&] { capture.configure({}, 10); });
    rejects([&] { capture.configure(manifest, 9000); });
    check(capture.configure(manifest, 128) == 3);
    auto result = capture.generate("hello", nullptr, 1, 0, 0);
    check(result.text == "OK" && result.tokens.size() == 2 && result.leaves.size() == 6 && result.ubatch.size() == 2);
    auto pair = capture.replay(result, 1, 0, 1);
    check(pair.first == std::vector<std::uint8_t>{42} && pair.second == std::vector<std::uint8_t>{43});
    for (mode = 3; mode <= 10; ++mode)
      rejects([&] { capture.generate("hello", nullptr, 1, 0, 0); });
    mode = 0;
    const auto before = calls;
    rejects([&] { capture.generate(std::string("bad\0prompt", 10), nullptr, 1, 0, 0); });
    rejects([&] { capture.generate("hello", nullptr, 0, 0, 0); });
    rejects([&] { capture.replay(result, 2, 0, 1); });
    rejects([&] { capture.replay(result, 1, 3, 1); });
    auto incomplete = result; incomplete.leaves.pop_back();
    rejects([&] { capture.replay(incomplete, 1, 0, 1); });
    check(calls == before); // Invalid inputs never reach the native API.
    for (mode = 11; mode <= 12; ++mode) rejects([&] { capture.replay(result, 1, 0, 1); });
    mode = 1; rejects([&] { capture.configure(manifest, 128); });
    mode = 0; rejects([&] { capture.generate("hello", nullptr, 1, 0, 0); });
    mode = 2; rejects([&] { capture.configure(manifest, 128); });
    mode = 0; capture.configure(manifest, 128);
    std::array<std::int32_t,1> negative{-1}; TracePrefix prefix{negative, 1, "rest"};
    rejects([&] { capture.generate("hello", &prefix, 1, 0, 0); });
    std::cout << "ABI6_TRACE_ADAPTER_CPU=PASS negative_cases=" << checks
              << " capture_replay=MOCK gpu=NOT_USED\n";
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
