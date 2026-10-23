// Native Vulkan inference adapter, using the official miner's llama.cpp b10015 pin.
// This is a distinct experimental ABI, not the stock CUDA wrapper or a CUDA translator.
#include "llama.h"
#include "ggml-backend.h"
#include "ggml-vulkan.h"
#include "keryx/nexus/vulkan_device_policy.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
thread_local std::string error;
std::atomic_bool cancelled{false};
static_assert(std::atomic_bool::is_always_lock_free);
struct Runtime {
    llama_model *model{};
    llama_context *context{};
    llama_sampler *sampler{};
    ggml_backend_dev_t devices[2]{};
    std::mutex mutex;
    ~Runtime() {
        if (sampler) llama_sampler_free(sampler);
        if (context) llama_free(context);
        if (model) llama_model_free(model);
    }
};
// Never forward model metadata, prompts, generated text or per-token logs.
void quiet(enum ggml_log_level, const char *, void *) {}
}

extern "C" {
int nexus_vulkan_inference_abi() { return 1; }
void nexus_vulkan_inference_cancel() { cancelled.store(true, std::memory_order_relaxed); }
const char *nexus_vulkan_inference_error() { return error.c_str(); }
void *nexus_vulkan_inference_load(const char *model, const char *pci, int context) {
    error.clear();
    cancelled.store(false, std::memory_order_relaxed);
    try {
        const bool nvidia_test=keryx::nexus::nvidia_vulkan_selected();
        const char* icd=std::getenv("VK_DRIVER_FILES");
        const char* legacy=std::getenv("VK_ICD_FILENAMES");
        const char* layers=std::getenv("VK_LOADER_LAYERS_DISABLE");
        const char* expected_icd=nvidia_test?"/usr/share/vulkan/icd.d/nvidia_icd.json":"/usr/share/vulkan/icd.d/radeon_icd.json";
        if(!icd||std::string(icd)!=expected_icd||!legacy||std::string(legacy)!=icd||!layers||std::string(layers)!="~implicit~")
            throw std::invalid_argument("Explicit device-scoped Vulkan ICD required");
        if (!model || !pci || std::string(pci) != (nvidia_test?"0000:00:01.0":"0000:00:00.0") || context < 1024 || context > 8192)
            throw std::invalid_argument("Explicit authorized secondary AMD GPU and bounded context required");
        llama_log_set(quiet, nullptr);
        ggml_log_set(quiet, nullptr);
        llama_backend_init();
        auto r = std::make_unique<Runtime>();
        const auto registry = ggml_backend_vk_reg();
        if (!registry) throw std::runtime_error("Vulkan backend unavailable");
        if(nvidia_test && ggml_backend_reg_dev_count(registry)!=1)
            throw std::runtime_error("NVIDIA replay requires one visible Vulkan device");
        for (size_t i = 0; i < ggml_backend_reg_dev_count(registry); ++i) {
            const auto device = ggml_backend_reg_dev_get(registry, i);
            ggml_backend_dev_props props{};
            ggml_backend_dev_get_props(device, &props);
            if (props.device_id && std::string(props.device_id) == pci &&
                props.type == GGML_BACKEND_DEVICE_TYPE_GPU) {
                if (r->devices[0]) throw std::runtime_error("Ambiguous Vulkan PCI identity");
                r->devices[0] = device;
            }
        }
        if (!r->devices[0]) throw std::runtime_error("Authorized PCI device not exposed by Vulkan");
        auto mp = llama_model_default_params();
        mp.devices = r->devices;
        mp.n_gpu_layers = 999;
        mp.split_mode = LLAMA_SPLIT_MODE_NONE;
        mp.main_gpu = 0;
        mp.use_mmap = nvidia_test;
        mp.use_direct_io = !nvidia_test;
        // Keep CPU-only tensors pageable in the NVIDIA replay. Vulkan pinned
        // host buffers otherwise pin a large CPU tensor allocation during load.
        // Model bytes, GPU offload and sampling remain identical.
        mp.no_host = nvidia_test;
        mp.progress_callback = [](float, void *) { return !cancelled.load(std::memory_order_relaxed); };
        const auto model_start=std::chrono::steady_clock::now();
        r->model = llama_model_load_from_file(model, mp);
        const auto model_end=std::chrono::steady_clock::now();
        if (!r->model) throw std::runtime_error("Official model cannot be loaded on the selected Vulkan GPU");
        auto cp = llama_context_default_params();
        cp.n_ctx = context;
        cp.n_batch = std::min(512, context);
        cp.n_ubatch = 128;
        cp.n_threads = cp.n_threads_batch = 2;
        cp.abort_callback = [](void *) { return cancelled.load(std::memory_order_relaxed); };
        cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
        cp.type_k = cp.type_v = GGML_TYPE_Q8_0;
        r->context = llama_init_from_model(r->model, cp);
        if (!r->context) {
            cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_AUTO;
            cp.type_k = cp.type_v = GGML_TYPE_F16;
            r->context = llama_init_from_model(r->model, cp);
        }
        if (!r->context) throw std::runtime_error("Selected Vulkan inference context allocation failed");
        if(nvidia_test)std::cerr<<"VULKAN_INFERENCE_LOAD_PROFILE model_ms="
            <<std::chrono::duration<double,std::milli>(model_end-model_start).count()
            <<" cpu_weight_storage="<<(nvidia_test?"pageable":"default")<<" context_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-model_end).count()<<'\n';
        // Match the stock sampling chain; output equality across GPU backends is NOT presumed.
        static const char *breakers[] = {"\n", ":", "\"", "*"};
        r->sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(r->sampler, llama_sampler_init_dry(llama_model_get_vocab(r->model),
            llama_model_n_ctx_train(r->model), 0.8f, 1.75f, 2, -1, breakers, 4));
        llama_sampler_chain_add(r->sampler, llama_sampler_init_penalties(256, 1.10f, 0, 0));
        llama_sampler_chain_add(r->sampler, llama_sampler_init_top_p(0.9f, 1));
        llama_sampler_chain_add(r->sampler, llama_sampler_init_temp(0.7f));
        llama_sampler_chain_add(r->sampler, llama_sampler_init_dist(42));
        return r.release();
    } catch (const std::exception &e) { error = e.what(); return nullptr; }
}
int nexus_vulkan_inference_generate(void *handle, const char *prompt, int tokens, char *out, int cap) {
    error.clear();
    try {
        if (!handle || !prompt || !out || tokens < 1 || tokens > 4096 || cap < 2 || cap > 65536)
            throw std::invalid_argument("Invalid bounded inference arguments");
        auto &r = *static_cast<Runtime *>(handle);
        std::lock_guard<std::mutex> lock(r.mutex);
        const size_t length = strnlen(prompt, 60 * 1024 + 1);
        if (!length || length > 60 * 1024) throw std::invalid_argument("Invalid prompt length");
        auto vocab = llama_model_get_vocab(r.model);
        std::vector<llama_token> input(length + 16);
        int n = llama_tokenize(vocab, prompt, length, input.data(), input.size(), true, true);
        if (n <= 0 || n > int(llama_n_ctx(r.context)) - 16) throw std::runtime_error("Prompt does not fit context");
        llama_memory_clear(llama_get_memory(r.context), true);
        llama_sampler_reset(r.sampler);
        int batch_size = llama_n_batch(r.context);
        const auto prefill_start=std::chrono::steady_clock::now();
        for (int i = 0; i < n; i += batch_size) {
            if (cancelled.load(std::memory_order_relaxed)) throw std::runtime_error("Vulkan inference cooperatively cancelled");
            auto batch = llama_batch_get_one(input.data() + i, std::min(batch_size, n - i));
            if (llama_decode(r.context, batch)) throw std::runtime_error("Vulkan prompt decode failed");
        }
        const auto decode_start=std::chrono::steady_clock::now();
        int written = 0, generated = 0;
        for (int i = 0; i < tokens; ++i) {
            if (cancelled.load(std::memory_order_relaxed)) throw std::runtime_error("Vulkan inference cooperatively cancelled");
            if (llama_memory_seq_pos_max(llama_get_memory(r.context), 0) + 1 >= int(llama_n_ctx(r.context))) break;
            auto token = llama_sampler_sample(r.sampler, r.context, -1);
            if (llama_vocab_is_eog(vocab, token)) break;
            char piece[256];
            int count = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, true);
            if (count < 0 || written + count >= cap - 1) throw std::runtime_error("Inference output exceeds bounds");
            memcpy(out + written, piece, count); written += count;
            auto batch = llama_batch_get_one(&token, 1);
            if (llama_decode(r.context, batch)) throw std::runtime_error("Vulkan token decode failed");
            ++generated;
        }
        if(std::getenv("NEXUS_VULKAN_TEST_UUID"))std::cerr<<"VULKAN_INFERENCE_GENERATE_PROFILE prefill_ms="
            <<std::chrono::duration<double,std::milli>(decode_start-prefill_start).count()
            <<" decode_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-decode_start).count()
            <<" generated_tokens="<<generated<<'\n';
        out[written] = 0;
        return written;
    } catch (const std::exception &e) { error = e.what(); return -1; }
}
void nexus_vulkan_inference_free(void *handle) { delete static_cast<Runtime *>(handle); }
}
