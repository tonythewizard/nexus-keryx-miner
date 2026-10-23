#include "keryx/nexus/developer_config.hpp"
// Offline-only inference runner. Prompts arrive over stdin, never argv or files.
#include "keryx/nexus/bounded_file_sha256.hpp"
#include "keryx/nexus/vulkan_device_policy.hpp"
#include <dlfcn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Cancel = void(*)();
std::atomic<Cancel> cancel_inference{nullptr};
static_assert(std::atomic<Cancel>::is_always_lock_free);
void interrupt(int) { if(auto cancel=cancel_inference.load(std::memory_order_relaxed)) cancel(); }

struct Handles {
    int lease{-1}; void *library{}, *runtime{}; void (*free_model)(void *){};
    void release() {
        cancel_inference.store(nullptr);
        if(runtime && free_model) {
            auto *owned=runtime;runtime=nullptr;free_model(owned);
            std::cerr << "VULKAN_INFERENCE_RELEASE model=RELEASED\n";
        }
        if(library) { auto *owned=library;library=nullptr;dlclose(owned); }
        if(lease>=0) { close(lease);lease=-1; }
    }
    ~Handles() { release(); }
};
template<class T> T sym(void *library, const char *name) {
    auto pointer = reinterpret_cast<T>(dlsym(library, name));
    if(!pointer) throw std::runtime_error("Missing Vulkan inference ABI symbol");
    return pointer;
}
int main(int argc, char **argv) {
    try {
        if(argc != 6 && !(argc==7 && std::string_view(argv[6])=="--lease-fd=3")) throw std::invalid_argument("usage: nexus-vulkan-inference-probe library library_sha256 model tokens context (prompt on stdin)");
        const auto equals = [](const char *name, const char *value) {
            const char *actual = std::getenv(name); return actual && std::string(actual) == value;
        };
        const bool nvidia_test=keryx::nexus::nvidia_vulkan_selected();
        const std::string pci=nvidia_test?"0000:00:01.0":"0000:00:00.0";
        const char* icd=nvidia_test?"/usr/share/vulkan/icd.d/nvidia_icd.json":"/usr/share/vulkan/icd.d/radeon_icd.json";
        if(!equals("VK_DRIVER_FILES",icd)||!equals("VK_ICD_FILENAMES",icd)||!equals("VK_LOADER_LAYERS_DISABLE","~implicit~"))
            throw std::runtime_error("Explicit device-scoped Vulkan ICD required");
        std::ifstream vendor("/sys/bus/pci/devices/"+pci+"/vendor"), device("/sys/bus/pci/devices/"+pci+"/device");
        std::string v,d;vendor>>v;device>>d;
        if(v!=(nvidia_test?"0x10de":"0x1002")||d!=(nvidia_test?"0x2482":"0x67df"))
            throw std::runtime_error("Authorized GPU PCI identity is absent");
        if(nvidia_test) {
            std::ifstream info("/proc/driver/nvidia/gpus/"+pci+"/information");
            const std::string value((std::istreambuf_iterator<char>(info)),{});
            if(value.find("" NEXUS_PRIMARY_GPU_UUID "")==std::string::npos)
                throw std::runtime_error("NVIDIA test UUID does not match selected PCI identity");
        }
        const std::string lease_path=(keryx::nexus::developer_data_root()/"state/gpu-devices"/
            keryx::nexus::developer_device_lease_name(nvidia_test)).string();
        size_t used{}; const int tokens = std::stoi(argv[4], &used);
        if(used != std::string(argv[4]).size() || tokens < 1 || tokens > 4096) throw std::invalid_argument("Invalid token bound");
        const int context = std::stoi(argv[5], &used);
        if(used != std::string(argv[5]).size() || context < 1024 || context > 8192) throw std::invalid_argument("Invalid context bound");
        std::vector<char> prompt(60 * 1024 + 1);
        std::cin.read(prompt.data(), prompt.size()); const auto count = std::cin.gcount();
        if(count <= 0 || count > 60 * 1024 || std::string_view(prompt.data(),count).find('\0') != std::string_view::npos)
            throw std::invalid_argument("Invalid bounded prompt");
        prompt[count] = 0;
        Handles h;
        h.lease = argc==7 ? fcntl(3,F_DUPFD_CLOEXEC,4) : open(lease_path.c_str(), O_CREAT|O_RDWR|O_NOFOLLOW,0600);
        if(argc==7){
            struct stat original{},canonical{};
            const char *path=lease_path.c_str();
            const int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
            const bool valid=fd>=0&&!fstat(fd,&canonical)&&!fstat(3,&original)&&
                canonical.st_dev==original.st_dev&&canonical.st_ino==original.st_ino;
            if(fd>=0)close(fd);
            if(!valid)throw std::runtime_error("Inherited AMD lease does not match the canonical device lock");
        }
        struct stat stat{};
        if(h.lease < 0 || fstat(h.lease,&stat) || !S_ISREG(stat.st_mode) || stat.st_uid != getuid() || (stat.st_mode&0077) || flock(h.lease,LOCK_EX|LOCK_NB))
            throw std::runtime_error("secondary AMD GPU lease unavailable; no concurrent GPU workload allowed");
        using keryx::nexus::bounded_file_sha256;
        if(bounded_file_sha256(argv[1]) != argv[2]) throw std::runtime_error("Vulkan adapter SHA-256 mismatch");
        if(bounded_file_sha256(argv[3]) != "946072b16f5d672e60357410f900888f6522b3ae49f241b5bf2142cb89637fb6")
            throw std::runtime_error("Official GGUF SHA-256 mismatch");
        h.library = dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
        if(!h.library) throw std::runtime_error("Vulkan inference library cannot be loaded");
        if(sym<int(*)()>(h.library,"nexus_vulkan_inference_abi")() != 1) throw std::runtime_error("Vulkan inference ABI mismatch");
        cancel_inference.store(sym<Cancel>(h.library,"nexus_vulkan_inference_cancel"));
        std::signal(SIGINT,interrupt); std::signal(SIGTERM,interrupt);
        auto load = sym<void*(*)(const char*,const char*,int)>(h.library,"nexus_vulkan_inference_load");
        auto generate = sym<int(*)(void*,const char*,int,char*,int)>(h.library,"nexus_vulkan_inference_generate");
        auto error = sym<const char*(*)()>(h.library,"nexus_vulkan_inference_error");
        h.free_model = sym<void(*)(void*)>(h.library,"nexus_vulkan_inference_free");
        h.runtime = load(argv[3],pci.c_str(),context);
        if(!h.runtime) throw std::runtime_error(error());
        std::vector<char> answer(65536);
        const int length = generate(h.runtime,prompt.data(),tokens,answer.data(),answer.size());
        if(length <= 0 || length >= int(answer.size())) throw std::runtime_error(error());
        // Do not report success until device/model and library teardown finish.
        h.release();
        std::cout.write(answer.data(),length);
        std::cerr << "VULKAN_NATIVE_INFERENCE=PASS backend=Vulkan pci="<<pci<<" official_model=true bytes=" << length
                  << " mainnet_transactions=0 production_readiness=NOT_ATTESTED\n";
        return 0;
    } catch(const std::exception &e) { std::cerr << "VULKAN_NATIVE_INFERENCE=FAIL " << e.what() << '\n'; return 1; }
}
