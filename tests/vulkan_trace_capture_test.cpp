// CPU-only graph/capture contracts. Raw GPU I/O is explicitly mocked here.
#include "../experiments/vulkan_inference/trace_capture.hpp"
#include <iostream>
#include <functional>
#include <limits>
using namespace nexus_trace_fixture;
static int status = 288;
static std::string armed;
extern "C" int nexus_vk_q8_probe_arm_for(const char * name) { armed = name; return 0; }
extern "C" int nexus_vk_q8_probe_read(uint8_t * out, size_t capacity) {
    if (status <= 0) return status;
    require(capacity == 288, "mock shape"); memset(out, 1, capacity); return status;
}
extern "C" void __wrap_ggml_backend_tensor_get(const ggml_tensor * t, void * out, size_t offset, size_t bytes) {
    memcpy(out, static_cast<const uint8_t *>(t->data) + offset, bytes);
}
static void rejected(const std::function<void()> & fn) {
    bool threw = false; try { fn(); } catch (...) { threw = true; } require(threw, "Invalid fixture accepted");
}
int main(int argc, char ** argv) {
    try {
        require(argc == 3, "usage: trace_capture_test MANIFEST NEW_PERSISTENT_DIRECTORY");
        std::ifstream file(argv[1], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        auto entries = manifest(bytes); require(entries.size() == 249, "tier0 product count");
        auto bad = bytes; bad.pop_back(); rejected([&] { manifest(bad); });
        bad = bytes; bad[0] ^= 1; rejected([&] { manifest(bad); });
        bad = bytes; bad.push_back(0); rejected([&] { manifest(bad); });
        rejected([&] { mmvq({}, 256); });
        std::vector<uint8_t> raw(288); for (size_t i = 0; i < raw.size(); ++i) raw[i] = i;
        auto converted = mmvq(raw, 256); require(converted.size() == 576, "canonical padding");
        require(!memcmp(converted.data(), raw.data(), 4) && !memcmp(converted.data() + 4, raw.data() + 16, 32), "Q8 byte reordering");
        require(std::all_of(converted.begin() + 288, converted.end(), [](auto v) { return v == 0; }), "Q8 padding bytes");
        raw[0] = 0; raw[1] = 0x7c; rejected([&] { mmvq(raw, 256); });
        auto root = std::filesystem::path(argv[2]); require(root.is_absolute() && !std::filesystem::exists(root), "new persistent directory required");
        std::filesystem::create_directories(root);
        Product p{"test.weight", 0, 13, UINT32_MAX, UINT32_MAX, 256, 4, 0, 0, 0};
        Capture c({p}, root);
        ggml_tensor w{}, x{}, y{};
        w.type = GGML_TYPE_Q5_K; w.ne[0] = 256; w.ne[1] = 4; w.ne[2] = w.ne[3] = 1; strcpy(w.name, p.name.c_str());
        x.type = GGML_TYPE_F32; x.ne[0] = 256; x.ne[1] = x.ne[2] = x.ne[3] = 1;
        x.nb[0] = 4; x.nb[1] = x.nb[2] = x.nb[3] = 1024;
        y.op = GGML_OP_MUL_MAT; y.type = GGML_TYPE_F32; y.src[0] = &w; y.src[1] = &x;
        float result[4] = {1, 2, 3, 4}; y.data = result;
        y.ne[0] = 4; y.ne[1] = y.ne[2] = y.ne[3] = 1;
        y.nb[0] = 4; y.nb[1] = y.nb[2] = y.nb[3] = 16;
        c.begin(0); require(Capture::callback(&y, true, &c) && armed == p.name, "weight-bound arm");
        require(Capture::callback(&y, false, &c), "capture callback"); c.finish();
        require(std::filesystem::file_size(root / "0/0.x") == 576 && std::filesystem::file_size(root / "0/0.y") == 16, "persisted capture dimensions");
        c.begin(1); status = -2; require(Capture::callback(&y, true, &c), "negative arm");
        require(!Capture::callback(&y, false, &c), "unsupported Q8 accepted"); rejected([&] { c.finish(); });
        Capture incomplete({p}, root); incomplete.begin(2); rejected([&] { incomplete.finish(); });
        Capture duplicate({p}, root); duplicate.begin(3); status = 288;
        require(Capture::callback(&y, true, &duplicate) && Capture::callback(&y, false, &duplicate), "first capture");
        require(!Capture::callback(&y, false, &duplicate), "duplicate capture accepted"); rejected([&] { duplicate.finish(); });
        Capture binding({p}, root); binding.begin(4); w.ne[0] = 512;
        require(!Capture::callback(&y, true, &binding), "bad weight binding accepted"); rejected([&] { binding.finish(); });
        Capture nonfinite({p}, root); nonfinite.begin(5); w.ne[0] = 256; result[0] = std::numeric_limits<float>::quiet_NaN();
        require(Capture::callback(&y, true, &nonfinite) && !Capture::callback(&y, false, &nonfinite), "non-finite output accepted");
        rejected([&] { nonfinite.finish(); });
        std::cout << "TRACE_CAPTURE_CPU_CONTRACTS=PASS GPU_IO=MOCKED\n";
        return 0;
    } catch (const std::exception & e) { std::cerr << e.what() << '\n'; return 1; }
}
