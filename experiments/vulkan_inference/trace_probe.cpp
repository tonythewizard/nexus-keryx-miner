#include "keryx/nexus/developer_config.hpp"
// Full real-model capture fixture. No node, wallet, key or submission interface.
#include "trace_capture.hpp"
#include "llama.h"
#include "ggml-vulkan.h"
#include "keryx/nexus/bounded_file_sha256.hpp"
#include "keryx/nexus/scoped_file_cache_guard.hpp"
#include <atomic>
#include <csignal>
#include <memory>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

using namespace nexus_trace_fixture;
static std::atomic_bool cancelled{false};
static_assert(std::atomic_bool::is_always_lock_free);
static void interrupt(int) { cancelled.store(true, std::memory_order_relaxed); }
static void quiet(enum ggml_log_level, const char *, void *) {}
static std::vector<uint8_t> read(const std::filesystem::path & path, size_t cap) {
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "Missing fixture file");
    std::vector<uint8_t> data(cap + 1);
    in.read(reinterpret_cast<char *>(data.data()), data.size());
    const auto size = in.gcount(); require(size >= 0 && size_t(size) <= cap, "Oversized fixture file");
    data.resize(size); return data;
}
int main(int argc, char ** argv) {
    try {
        require(argc == 4 || argc == 5, "usage: nexus-vulkan-trace-probe MODEL MANIFEST NEW_DIRECTORY [PREFIX_TOKEN_FILE]");
        const auto env_is = [](const char * key, const char * value) {
            const auto * actual = getenv(key); return actual && std::string(actual) == value;
        };
        require(env_is("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/radeon_icd.json") &&
            env_is("VK_LOADER_LAYERS_DISABLE", "~implicit~") && env_is("NEXUS_VK_Q8_PROBE_SOFTWARE_DOT", "1") &&
            env_is("GGML_VK_FORCE_MMVQ", "1"), "Explicit AMD-only canonical Q8 fixture environment required");
        std::ifstream vendor("/sys/bus/pci/devices/0000:00:00.0/vendor"), device("/sys/bus/pci/devices/0000:00:00.0/device");
        std::string v, d; vendor >> v; device >> d;
        require(v == "0x1002" && d == "0x67df", "Authorized secondary AMD GPU identity mismatch");
        struct Lease { int fd{-1}; ~Lease() { if (fd >= 0) close(fd); } } lease;
        lease.fd = open((keryx::nexus::developer_data_root()/"state/gpu-devices"/keryx::nexus::developer_device_lease_name(false)).c_str(),
            O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        struct stat st{};
        require(lease.fd >= 0 && !fstat(lease.fd, &st) && S_ISREG(st.st_mode) && st.st_uid == getuid() &&
            !(st.st_mode & 0077) && !flock(lease.fd, LOCK_EX | LOCK_NB), "Exclusive secondary AMD GPU lease unavailable");
        const std::filesystem::path root(argv[3]);
        require(root.is_absolute() && !std::filesystem::exists(root), "New absolute capture directory required");
        auto bytes = read(argv[2], 1 << 20);
        auto products = manifest(bytes);
        require(products.size() == 249 && products[0].name == "blk.0.attn_gate.weight", "Only the audited Qwen tier-0 fixture is supported");
        keryx::nexus::ScopedFileCacheGuard cache_guard;
        require(keryx::nexus::bounded_file_sha256(argv[1]) == "946072b16f5d672e60357410f900888f6522b3ae49f241b5bf2142cb89637fb6",
            "Official Qwen GGUF SHA-256 mismatch");
        std::filesystem::create_directory(root);
        Capture capture(std::move(products), root);
        std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
        llama_log_set(quiet, nullptr); ggml_log_set(quiet, nullptr); llama_backend_init();
        auto registry = ggml_backend_vk_reg(); require(registry, "Vulkan registry unavailable");
        ggml_backend_dev_t devices[2]{};
        for (size_t i = 0; i < ggml_backend_reg_dev_count(registry); ++i) {
            auto dev = ggml_backend_reg_dev_get(registry, i); ggml_backend_dev_props props{};
            ggml_backend_dev_get_props(dev, &props);
            if (props.device_id && std::string(props.device_id) == "0000:00:00.0" && props.type == GGML_BACKEND_DEVICE_TYPE_GPU) {
                require(!devices[0], "Ambiguous Vulkan PCI device"); devices[0] = dev;
            }
        }
        require(devices[0], "Authorized Vulkan device unavailable");
        auto mp = llama_model_default_params();
        mp.devices = devices; mp.n_gpu_layers = 999; mp.split_mode = LLAMA_SPLIT_MODE_NONE;
        mp.use_mmap = false; mp.use_direct_io = true;
        mp.progress_callback = [](float, void *) { return !cancelled.load(std::memory_order_relaxed); };
        std::unique_ptr<llama_model, decltype(&llama_model_free)> model(llama_model_load_from_file(argv[1], mp), llama_model_free);
        require(bool(model), "Real Qwen model load failed");
        auto cp = llama_context_default_params();
        cp.n_ctx = 1024; cp.n_batch = cp.n_ubatch = 1; cp.n_threads = cp.n_threads_batch = 2;
        cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_AUTO;
        cp.cb_eval = Capture::callback; cp.cb_eval_user_data = &capture;
        cp.abort_callback = [](void *) { return cancelled.load(std::memory_order_relaxed); };
        std::unique_ptr<llama_context, decltype(&llama_free)> ctx(llama_init_from_model(model.get(), cp), llama_free);
        require(bool(ctx), "Real Qwen context allocation failed");
        const auto * vocab = llama_model_get_vocab(model.get());
        const std::string prompt = "Say OK.";
        std::vector<llama_token> tokens(64);
        const int count = llama_tokenize(vocab, prompt.data(), prompt.size(), tokens.data(), tokens.size(), true, true);
        require(count > 0 && count < 16, "Unexpected fixture prompt tokenization"); tokens.resize(count);
        const bool prefix = argc == 5;
        if (prefix) {
            tokens.resize(6, tokens.back());
            const auto raw = read(argv[4], 128); require(raw.size() == 128, "Prefix must contain exactly 32 token ids");
            for (int i = 0; i < 32; ++i) {
                const auto id = le(raw, i * 4, 4);
                require(id < uint64_t(llama_vocab_n_tokens(vocab)), "Prefix token outside vocabulary");
                tokens.push_back(id);
            }
        }
        const auto n_prompt = tokens.size();
        auto decode = [&](llama_token token) {
            require(!cancelled.load(std::memory_order_relaxed), "Trace fixture cooperatively cancelled");
            capture.begin(capture.position);
            auto batch = llama_batch_get_one(&token, 1);
            const auto status = llama_decode(ctx.get(), batch);
            capture.finish(); require(status == 0, "Real Qwen decode failed"); ++capture.position;
        };
        for (auto token : tokens) decode(token);
        // Match the stock sampler chain. Only the fixture token budget is bounded to one.
        static const char * breakers[] = {"\n", ":", "\"", "*"};
        std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(
            llama_sampler_chain_init(llama_sampler_chain_default_params()), llama_sampler_free);
        llama_sampler_chain_add(sampler.get(), llama_sampler_init_dry(vocab, llama_model_n_ctx_train(model.get()), .8f, 1.75f, 2, -1, breakers, 4));
        llama_sampler_chain_add(sampler.get(), llama_sampler_init_penalties(256, 1.10f, 0, 0));
        llama_sampler_chain_add(sampler.get(), llama_sampler_init_top_p(.9f, 1));
        llama_sampler_chain_add(sampler.get(), llama_sampler_init_temp(.7f));
        llama_sampler_chain_add(sampler.get(), llama_sampler_init_dist(42));
        auto token = llama_sampler_sample(sampler.get(), ctx.get(), -1);
        std::string answer;
        if (!llama_vocab_is_eog(vocab, token)) {
            char piece[256]; const int length = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, true);
            require(length > 0 && length <= 32, "Unexpected single-token answer size");
            answer.assign(piece, length); tokens.push_back(token); decode(token);
        }
        cache_guard.check();
        std::ofstream metadata(root / "capture.tsv");
        metadata << "NEXUS_VULKAN_TRACE_FIXTURE_V1\n" << n_prompt << '\t' << tokens.size() << '\t' << answer.size() << '\t' << prompt.size() << '\t' << prefix << '\n';
        for (auto id : tokens) metadata << id << '\n';
        metadata.close(); require(bool(metadata), "Cannot persist trace metadata");
        // Only synthetic fixture text is persisted; no caller-supplied prompt is accepted.
        std::ofstream text(root / "answer.txt"); text << answer; text.close();
        require(bool(text), "Cannot persist fixture answer");
        sampler.reset(); ctx.reset(); model.reset();
        std::cout << "REAL_QWEN_TRACE_CAPTURE=COMPLETE positions=" << tokens.size() << " products=249 ubatch=1 prefix=" << prefix
            << " verifier=NOT_YET_RUN mainnet_transactions=0\n";
        return 0;
    } catch (const std::exception & e) { std::cerr << "REAL_QWEN_TRACE_CAPTURE=FAIL " << e.what() << '\n'; return 1; }
}
