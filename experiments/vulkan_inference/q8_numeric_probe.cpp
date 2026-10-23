#include "keryx/nexus/developer_config.hpp"
// Offline numerical prerequisite for canonical traces. No model, wallet or RPC.
#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-vulkan.h"
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" void nexus_vk_q8_probe_arm();
extern "C" int nexus_vk_q8_probe_read(uint8_t *, size_t);
namespace fs = std::filesystem;
static void require(bool condition, const char * message) {
    if (!condition) throw std::runtime_error(message);
}
static void write(const fs::path & path, const void * data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char *>(data), size);
    require(bool(out), "Cannot persist numerical fixture");
}
int main(int argc, char ** argv) {
    try {
        require(argc == 2, "usage: nexus-vulkan-q8-numeric-probe NEW_OUTPUT_DIRECTORY");
        const char * icd = std::getenv("VK_DRIVER_FILES"), * layers = std::getenv("VK_LOADER_LAYERS_DISABLE");
        require(icd && std::string(icd) == "/usr/share/vulkan/icd.d/radeon_icd.json" &&
            layers && std::string(layers) == "~implicit~", "AMD-only ICD and disabled implicit layers required");
        const fs::path output(argv[1]);
        require(output.is_absolute() && !fs::exists(output), "A new absolute output directory is required");
        std::ifstream vendor("/sys/bus/pci/devices/0000:00:00.0/vendor"), device("/sys/bus/pci/devices/0000:00:00.0/device");
        std::string vendor_id, device_id; vendor >> vendor_id; device >> device_id;
        require(vendor_id == "0x1002" && device_id == "0x67df", "Authorized secondary AMD GPU PCI identity is absent");
        const int lease = open((keryx::nexus::developer_data_root()/"state/gpu-devices"/keryx::nexus::developer_device_lease_name(false)).c_str(),
            O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct stat st{};
        require(lease >= 0 && !fstat(lease, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
            !(st.st_mode & 0077) && !flock(lease, LOCK_EX | LOCK_NB), "Exclusive AMD lease unavailable");
        auto registry = ggml_backend_vk_reg();
        require(registry, "Vulkan registry unavailable");
        ggml_backend_dev_t selected = nullptr;
        for (size_t i = 0; i < ggml_backend_reg_dev_count(registry); ++i) {
            auto dev = ggml_backend_reg_dev_get(registry, i);
            ggml_backend_dev_props props{}; ggml_backend_dev_get_props(dev, &props);
            if (props.device_id && std::string(props.device_id) == "0000:00:00.0" && props.type == GGML_BACKEND_DEVICE_TYPE_GPU) {
                require(!selected, "Ambiguous AMD PCI identity"); selected = dev;
            }
        }
        require(selected, "Authorized secondary AMD GPU unavailable");
        std::unique_ptr<ggml_backend, decltype(&ggml_backend_free)> backend(ggml_backend_dev_init(selected, nullptr), ggml_backend_free);
        require(bool(backend), "Vulkan backend initialization failed");
        fs::create_directory(output);
        std::ofstream index(output / "cases.tsv");
        int captured = 0, unsupported = 0;
        for (auto type : {GGML_TYPE_Q4_0, GGML_TYPE_Q5_0, GGML_TYPE_Q8_0, GGML_TYPE_Q4_K, GGML_TYPE_Q5_K, GGML_TYPE_Q6_K}) {
            constexpr int k = 2048, m = 64;
            const auto dir = output / ggml_type_name(type); fs::create_directory(dir);
            std::unique_ptr<ggml_context, decltype(&ggml_free)> ctx(ggml_init({2 * 1024 * 1024, nullptr, true}), ggml_free);
            require(bool(ctx), "GGML context failed");
            auto w = ggml_new_tensor_2d(ctx.get(), type, k, m);
            auto x = ggml_new_tensor_2d(ctx.get(), GGML_TYPE_F32, k, 1);
            ggml_set_name(w, "nexus.q8.probe.weight"); ggml_set_input(w); ggml_set_input(x);
            auto y = ggml_mul_mat(ctx.get(), w, x); ggml_set_output(y);
            auto graph = ggml_new_graph(ctx.get()); ggml_build_forward_expand(graph, y);
            std::unique_ptr<ggml_backend_buffer, decltype(&ggml_backend_buffer_free)> buffer(
                ggml_backend_alloc_ctx_tensors(ctx.get(), backend.get()), ggml_backend_buffer_free);
            require(bool(buffer), "Vulkan graph allocation failed");
            std::vector<float> weights(k * m), input(k), actual(m);
            for (int i = 0; i < k * m; ++i) weights[i] = std::sin(float(i) * .03125f) * .125f;
            for (int i = 0; i < k; ++i) input[i] = std::cos(float(i) * .0625f) + float(i % 17) / 31.f;
            std::vector<uint8_t> encoded(ggml_nbytes(w)), raw(k / 128 * 144);
            require(ggml_quantize_chunk(type, weights.data(), encoded.data(), 0, m, k, nullptr) == encoded.size(), "Quantization failed");
            ggml_backend_tensor_set(w, encoded.data(), 0, encoded.size());
            ggml_backend_tensor_set(x, input.data(), 0, input.size() * sizeof(float));
            nexus_vk_q8_probe_arm();
            require(ggml_backend_graph_compute(backend.get(), graph) == GGML_STATUS_SUCCESS, "Vulkan matmul failed");
            ggml_backend_tensor_get(y, actual.data(), 0, actual.size() * sizeof(float));
            for (float v : actual) require(std::isfinite(v), "Non-finite GPU output");
            const int rc = nexus_vk_q8_probe_read(raw.data(), raw.size());
            write(dir / "weights.bin", encoded.data(), encoded.size());
            write(dir / "input.f32", input.data(), input.size() * sizeof(float));
            write(dir / "output.f32", actual.data(), actual.size() * sizeof(float));
            if (rc > 0) {
                require(size_t(rc) == raw.size(), "Unexpected Q8 snapshot size");
                write(dir / "input.q8x4", raw.data(), raw.size()); ++captured;
            } else ++unsupported;
            index << ggml_type_name(type) << '\t' << int(type) << '\t' << k << '\t' << m << '\t' << rc << '\n';
            std::cout << "VULKAN_Q8_CAPTURE type=" << ggml_type_name(type) << " code=" << rc << '\n';
        }
        require(bool(index), "Cannot persist fixture index");
        backend.reset(); close(lease);
        std::cout << "CAPTURED=" << captured << " UNSUPPORTED=" << unsupported << " MAINNET_TRANSACTIONS=0\n";
        return unsupported ? 4 : 0;
    } catch (const std::exception & e) { std::cerr << "VULKAN_Q8_PROBE=FAIL " << e.what() << '\n'; return 1; }
}
