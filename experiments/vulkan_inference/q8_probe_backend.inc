// Diagnostic only: capture the actual Vulkan MMVQ input before scratch reuse.
// Included before ggml_vk_mul_mat_vec_q_f16 in the pinned b10015 dispatcher.
// Not a response trace producer and never linked into the release runtime.
struct nexus_vk_q8_probe_state {
    bool armed = false;
    int status = -1;
    uint32_t matches = 0;
    std::string weight = "nexus.q8.probe.weight";
    std::vector<uint8_t> bytes;
    vk_buffer staging;
};
static thread_local nexus_vk_q8_probe_state nexus_vk_q8_probe;

extern "C" void nexus_vk_q8_probe_arm() {
    nexus_vk_q8_probe = {};
    nexus_vk_q8_probe.armed = true;
}

extern "C" int nexus_vk_q8_probe_arm_for(const char * weight) {
    if (!weight || !*weight || strlen(weight) >= GGML_MAX_NAME) return -1;
    nexus_vk_q8_probe_arm();
    nexus_vk_q8_probe.weight = weight;
    return 0;
}

extern "C" int nexus_vk_q8_probe_read(uint8_t * out, size_t capacity) {
    auto & p = nexus_vk_q8_probe;
    if (p.armed) return -6; // Never expose a scheduled but unfinished GPU copy.
    if (p.status != 0) return p.status;
    if (!out || capacity < p.bytes.size()) return -4;
    memcpy(out, p.bytes.data(), p.bytes.size());
    return static_cast<int>(p.bytes.size());
}

static void nexus_vk_q8_probe_capture(ggml_backend_vk_context * ctx, vk_context & subctx,
        const ggml_tensor * w, const ggml_tensor * x, const vk_subbuffer & q, bool quantized) {
    auto & p = nexus_vk_q8_probe;
    if (!p.armed || p.weight != w->name) return;
    if (++p.matches != 1) { p.status = -3; return; }
    if (!quantized || ggml_nrows(x) != 1 || x->ne[0] < 1 || x->ne[0] > 32768) {
        p.status = -2; return;
    }
    const size_t size = ((x->ne[0] + 127) / 128) * 144;
    if (size > q.size) { p.status = -5; return; }
    p.bytes.resize(size);
    p.staging = ggml_vk_create_buffer_check(ctx->device, size,
        {vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent});
    auto source = q.buffer;
    ggml_vk_sync_buffers(ctx, subctx);
    ggml_vk_buffer_copy_async(subctx, p.staging, 0, source, q.offset, size);
    // The copy must complete before the next matmul can overwrite prealloc_y.
    ggml_vk_sync_buffers(ctx, subctx);
    p.status = 0;
}

static void nexus_vk_q8_probe_finish(ggml_backend_vk_context * ctx) {
    auto & p = nexus_vk_q8_probe;
    if (!p.armed) return;
    ggml_vk_synchronize(ctx);
    if (p.status == 0 && p.staging) memcpy(p.bytes.data(), p.staging->ptr, p.bytes.size());
    p.staging.reset();
    p.armed = false;
}
