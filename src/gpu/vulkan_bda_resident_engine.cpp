#include "keryx/nexus/consensus/vulkan_bda_resident_engine.hpp"
#include "keryx/nexus/consensus/pom_v4_bda_spv.hpp"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <iostream>
#include <array>
#include <exception>

#define VK_CHECK(call) \
    do { \
        VkResult res = (call); \
        if (res != VK_SUCCESS) { \
            throw std::runtime_error("Vulkan error " + std::to_string(res) + \
                                     " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)

namespace keryx::nexus::vulkan {

namespace {

struct InternalBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
    bool is_cached = false;
};

uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type!");
}

InternalBuffer create_buffer(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size,
                             VkBufferUsageFlags usage, VkMemoryPropertyFlags preferred_flags, bool map) {
    InternalBuffer buf;
    buf.size = size;

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &buf.buffer));

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(device, buf.buffer, &mem_reqs);

    uint32_t mem_type = 0;
    try {
        mem_type = find_memory_type(physical_device, mem_reqs.memoryTypeBits, preferred_flags);
        buf.is_cached = (preferred_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
    } catch (...) {
        if (preferred_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            mem_type = find_memory_type(physical_device, mem_reqs.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            buf.is_cached = false;
        } else {
            throw;
        }
    }

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = mem_type;

    VK_CHECK(vkAllocateMemory(device, &alloc_info, nullptr, &buf.memory));
    VK_CHECK(vkBindBufferMemory(device, buf.buffer, buf.memory, 0));
    if (map) {
        VK_CHECK(vkMapMemory(device, buf.memory, 0, size, 0, &buf.mapped));
    }

    return buf;
}

void destroy_buffer(VkDevice device, InternalBuffer &buf) {
    if (buf.mapped) {
        vkUnmapMemory(device, buf.memory);
        buf.mapped = nullptr;
    }
    if (buf.buffer) {
        vkDestroyBuffer(device, buf.buffer, nullptr);
        buf.buffer = VK_NULL_HANDLE;
    }
    if (buf.memory) {
        vkFreeMemory(device, buf.memory, nullptr);
        buf.memory = VK_NULL_HANDLE;
    }
    buf.size = 0;
    buf.is_cached = false;
}

struct PushConstants {
    uint64_t bank0_addr;
    uint64_t bank1_addr;
    uint64_t ntiles;
    uint64_t split_tiles;
    uint32_t diagnostics;
    uint32_t write_offsets;
    uint64_t p0;
    uint64_t p1;
    uint64_t p2;
    uint64_t p3;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;
    uint64_t inv_ntiles;
};

} // namespace

struct VulkanBdaResidentEngine::Impl {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkQueue queue = VK_NULL_HANDLE;

    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // Fast GPU buffers & DMA staging
    InternalBuffer buf_seeds;         // Host visible for fast CPU seeds push
    InternalBuffer buf_offsets_dev;   // Fast VRAM device-local buffer
    InternalBuffer buf_offsets_host;  // Host visible cached staging buffer
    InternalBuffer buf_finals_dev;    // Fast VRAM device-local buffer
    InternalBuffer buf_finals_host;   // Host visible cached staging buffer
    InternalBuffer buf_trace;         // Host visible diagnostics buffer
    InternalBuffer buf_tile_folds;    // Fast VRAM device-local precomputed fold table (47.7 MB)
    InternalBuffer buf_winner;        // Host visible atomic winner buffer (64 bytes)

    uint32_t max_batch_size = 18432;
    uint64_t model_tiles = 0;
    uint64_t split_tiles = 0;
    uint64_t bank0_addr = 0;
    uint64_t bank1_addr = 0;

    bool ready = false;
    bool paused = false;
    BdaEngineStats stats;

    Impl(VkDevice dev, VkPhysicalDevice pdev, uint32_t qfam, VkQueue q, const std::vector<uint32_t> &spirv)
        : device(dev), physical_device(pdev), queue_family(qfam), queue(q) {

        // Shader Module
        VkShaderModuleCreateInfo mod_info{};
        mod_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mod_info.codeSize = spirv.size() * sizeof(uint32_t);
        mod_info.pCode = spirv.data();
        VK_CHECK(vkCreateShaderModule(device, &mod_info, nullptr, &shader));

        // 6 Descriptors: seeds (0), offsets (1), finals (2), trace (3), tile_folds (4), winner (5)
        std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
        for (uint32_t i = 0; i < 6; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        VK_CHECK(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &descriptor_layout));

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &descriptor_layout;
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;
        VK_CHECK(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout));

        VkComputePipelineCreateInfo pipeline_create_info{};
        pipeline_create_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_create_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_create_info.stage.module = shader;
        pipeline_create_info.stage.pName = "main";
        pipeline_create_info.layout = pipeline_layout;
        VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_create_info, nullptr, &pipeline));

        // Commands & Synchronization
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family;
        VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &cmd_pool));

        VkCommandBufferAllocateInfo cmd_alloc{};
        cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_alloc.commandPool = cmd_pool;
        cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_alloc.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &cmd_alloc, &cmd_buffer));

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &fence));

        // Allocate fast buffers: device local VRAM for compute + cached host staging
        buf_seeds = create_buffer(device, physical_device, max_batch_size * sizeof(uint64_t),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);

        buf_offsets_dev = create_buffer(device, physical_device, max_batch_size * 256 * sizeof(uint64_t),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
        buf_offsets_host = create_buffer(device, physical_device, max_batch_size * 256 * sizeof(uint64_t),
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true);

        buf_finals_dev = create_buffer(device, physical_device, max_batch_size * 1024,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
        buf_finals_host = create_buffer(device, physical_device, max_batch_size * 1024,
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true);

        // Diagnostics is only run on single winning nonce (1 nonce * 257 * 1024)
        buf_trace = create_buffer(device, physical_device, 16 * 257 * 1024,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);

        buf_winner = create_buffer(device, physical_device, 64,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    }

    ~Impl() {
        if (device) {
            vkDeviceWaitIdle(device);

            destroy_buffer(device, buf_seeds);
            destroy_buffer(device, buf_offsets_dev);
            destroy_buffer(device, buf_offsets_host);
            destroy_buffer(device, buf_finals_dev);
            destroy_buffer(device, buf_finals_host);
            destroy_buffer(device, buf_trace);
            destroy_buffer(device, buf_tile_folds);
            destroy_buffer(device, buf_winner);

            if (fence) vkDestroyFence(device, fence, nullptr);
            if (cmd_pool) vkDestroyCommandPool(device, cmd_pool, nullptr);
            if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (descriptor_layout) vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
            if (shader) vkDestroyShaderModule(device, shader, nullptr);
        }
    }

    void setup_descriptors() {
        if (descriptor_pool) {
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            descriptor_pool = VK_NULL_HANDLE;
        }

        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_size.descriptorCount = 6;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        VK_CHECK(vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool));

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = descriptor_pool;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &descriptor_layout;
        VK_CHECK(vkAllocateDescriptorSets(device, &alloc_info, &descriptor_set));

        std::array<InternalBuffer*, 6> all_buffers = {
            &buf_seeds, &buf_offsets_dev, &buf_finals_dev, &buf_trace, &buf_tile_folds, &buf_winner
        };

        std::array<VkDescriptorBufferInfo, 6> buffer_infos{};
        std::array<VkWriteDescriptorSet, 6> writes{};

        for (uint32_t i = 0; i < 6; ++i) {
            buffer_infos[i].buffer = all_buffers[i]->buffer;
            buffer_infos[i].offset = 0;
            buffer_infos[i].range = all_buffers[i]->size;

            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descriptor_set;
            writes[i].dstBinding = i;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].descriptorCount = 1;
            writes[i].pBufferInfo = &buffer_infos[i];
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
};

VulkanBdaResidentEngine::VulkanBdaResidentEngine(
    VkDevice device, VkPhysicalDevice physical_device,
    uint32_t compute_queue_family, VkQueue queue,
    const std::vector<uint32_t> &spirv_code)
    : impl_(std::make_unique<Impl>(device, physical_device, compute_queue_family, queue, spirv_code)) {}

VulkanBdaResidentEngine::~VulkanBdaResidentEngine() = default;

void VulkanBdaResidentEngine::attach_resident_tensors(
    const std::vector<BdaTensorDescriptor> &tensors,
    uint64_t total_model_tiles) {

    if (tensors.empty()) throw std::invalid_argument("Empty tensor list provided to attach_resident_tensors");

    auto &p = *impl_;
    p.model_tiles = total_model_tiles;
    p.bank0_addr = tensors[0].device_address;
    if (tensors.size() > 1) {
        p.split_tiles = tensors[0].bytes / 1024;
        p.bank1_addr = tensors[1].device_address;
    } else {
        p.split_tiles = total_model_tiles;
        p.bank1_addr = tensors[0].device_address;
    }

    // Allocate tile_folds buffer (47.7 MB) in device-local memory
    if (p.buf_tile_folds.buffer == VK_NULL_HANDLE) {
        p.buf_tile_folds = create_buffer(p.device, p.physical_device, total_model_tiles * sizeof(uint64_t),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);

        // Precompute tile folds on GPU in ~2.5 ms
        VkShaderModule folds_shader = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo folds_mod_info{};
        folds_mod_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        folds_mod_info.codeSize = sizeof(pom_v4_bda_prepare_folds_spv);
        folds_mod_info.pCode = pom_v4_bda_prepare_folds_spv;
        VK_CHECK(vkCreateShaderModule(p.device, &folds_mod_info, nullptr, &folds_shader));

        VkDescriptorSetLayoutBinding fold_bind{};
        fold_bind.binding = 0;
        fold_bind.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        fold_bind.descriptorCount = 1;
        fold_bind.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo fold_dsl_info{};
        fold_dsl_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        fold_dsl_info.bindingCount = 1;
        fold_dsl_info.pBindings = &fold_bind;
        VkDescriptorSetLayout fold_dsl;
        VK_CHECK(vkCreateDescriptorSetLayout(p.device, &fold_dsl_info, nullptr, &fold_dsl));

        VkPushConstantRange fold_pcr{};
        fold_pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        fold_pcr.offset = 0;
        fold_pcr.size = 32;

        VkPipelineLayoutCreateInfo fold_pl_info{};
        fold_pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        fold_pl_info.setLayoutCount = 1;
        fold_pl_info.pSetLayouts = &fold_dsl;
        fold_pl_info.pushConstantRangeCount = 1;
        fold_pl_info.pPushConstantRanges = &fold_pcr;
        VkPipelineLayout fold_pl;
        VK_CHECK(vkCreatePipelineLayout(p.device, &fold_pl_info, nullptr, &fold_pl));

        VkComputePipelineCreateInfo fold_pipe_info{};
        fold_pipe_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        fold_pipe_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fold_pipe_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        fold_pipe_info.stage.module = folds_shader;
        fold_pipe_info.stage.pName = "main";
        fold_pipe_info.layout = fold_pl;
        VkPipeline fold_pipe;
        VK_CHECK(vkCreateComputePipelines(p.device, VK_NULL_HANDLE, 1, &fold_pipe_info, nullptr, &fold_pipe));

        VkDescriptorPoolSize fold_ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo fold_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 1, 1, &fold_ps};
        VkDescriptorPool fold_pool;
        VK_CHECK(vkCreateDescriptorPool(p.device, &fold_pool_info, nullptr, &fold_pool));

        VkDescriptorSetAllocateInfo fold_ds_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, fold_pool, 1, &fold_dsl};
        VkDescriptorSet fold_ds;
        VK_CHECK(vkAllocateDescriptorSets(p.device, &fold_ds_alloc, &fold_ds));

        VkDescriptorBufferInfo fold_dbi{p.buf_tile_folds.buffer, 0, p.buf_tile_folds.size};
        VkWriteDescriptorSet fold_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, fold_ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &fold_dbi, nullptr};
        vkUpdateDescriptorSets(p.device, 1, &fold_write, 0, nullptr);

        struct {
            uint64_t bank0;
            uint64_t bank1;
            uint64_t ntiles;
            uint64_t split_tiles;
        } fold_pc{p.bank0_addr, p.bank1_addr, p.model_tiles, p.split_tiles};

        VK_CHECK(vkResetCommandBuffer(p.cmd_buffer, 0));
        VkCommandBufferBeginInfo cb_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(p.cmd_buffer, &cb_begin));
        vkCmdBindPipeline(p.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, fold_pipe);
        vkCmdBindDescriptorSets(p.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, fold_pl, 0, 1, &fold_ds, 0, nullptr);
        vkCmdPushConstants(p.cmd_buffer, fold_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fold_pc), &fold_pc);
        vkCmdDispatch(p.cmd_buffer, static_cast<uint32_t>((p.model_tiles + 127) / 128), 1, 1);
        VK_CHECK(vkEndCommandBuffer(p.cmd_buffer));

        VK_CHECK(vkResetFences(p.device, 1, &p.fence));
        VkSubmitInfo fold_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &p.cmd_buffer, 0, nullptr};
        VK_CHECK(vkQueueSubmit(p.queue, 1, &fold_submit, p.fence));
        VK_CHECK(vkWaitForFences(p.device, 1, &p.fence, VK_TRUE, 5000000000ULL));

        vkDestroyDescriptorPool(p.device, fold_pool, nullptr);
        vkDestroyPipeline(p.device, fold_pipe, nullptr);
        vkDestroyPipelineLayout(p.device, fold_pl, nullptr);
        vkDestroyDescriptorSetLayout(p.device, fold_dsl, nullptr);
        vkDestroyShaderModule(p.device, folds_shader, nullptr);
    }

    p.setup_descriptors();

    p.ready = true;
    p.paused = false;
}

bool VulkanBdaResidentEngine::run_batch(
    std::span<const uint64_t> seeds,
    bool diagnostics,
    bool copy_offsets,
    BdaWalkBatch &out,
    uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3,
    uint64_t t0, uint64_t t1, uint64_t t2, uint64_t t3,
    const std::function<bool()> &cancelled,
    const std::function<void()> &after_enqueue) {

    auto &p = *impl_;
    if (!p.ready || p.paused) return false;
    if (seeds.empty() || seeds.size() > p.max_batch_size) return false;
    if(diagnostics && seeds.size()>16)
        throw std::invalid_argument("BDA trace capacity exceeded");
    if (cancelled && cancelled()) return false;

    // Reset winner atomic to 0xFFFFFFFF
    if (p.buf_winner.mapped) {
        *static_cast<uint32_t*>(p.buf_winner.mapped) = 0xFFFFFFFFu;
    }

    // Copy seeds to host-visible buffer
    std::memcpy(p.buf_seeds.mapped, seeds.data(), seeds.size_bytes());
    // Four nonce lanes per workgroup. Tail lanes must never use a prior job's seeds.
    const auto padded=(seeds.size()+3)/4*4;
    std::fill(static_cast<uint64_t*>(p.buf_seeds.mapped)+seeds.size(),
              static_cast<uint64_t*>(p.buf_seeds.mapped)+padded,0);

    VK_CHECK(vkResetFences(p.device, 1, &p.fence));
    VK_CHECK(vkResetCommandBuffer(p.cmd_buffer, 0));

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(p.cmd_buffer, &begin_info));

    vkCmdBindPipeline(p.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vkCmdBindDescriptorSets(p.cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline_layout, 0, 1, &p.descriptor_set, 0, nullptr);

    const bool need_offsets = copy_offsets || diagnostics;
    PushConstants pc{
        .bank0_addr = p.bank0_addr,
        .bank1_addr = p.bank1_addr,
        .ntiles = p.model_tiles,
        .split_tiles = p.split_tiles,
        .diagnostics = diagnostics ? 1u : 0u,
        .write_offsets = need_offsets ? 1u : 0u,
        .p0 = p0, .p1 = p1, .p2 = p2, .p3 = p3,
        .t0 = t0, .t1 = t1, .t2 = t2, .t3 = t3,
        .inv_ntiles = (p.model_tiles > 0) ? (0xFFFFFFFFFFFFFFFFULL / p.model_tiles) : 0ULL
    };
    vkCmdPushConstants(p.cmd_buffer, p.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants), &pc);

    // Optimized multi-warp kernel: 4 nonces per workgroup (local_size_x = 128)
    uint32_t workgroups = static_cast<uint32_t>((seeds.size() + 3) / 4);
    vkCmdDispatch(p.cmd_buffer, workgroups, 1, 1);

    // Hardware DMA copy only needed for diagnostics or explicit offsets
    if (diagnostics || need_offsets) {
        VkBufferMemoryBarrier barriers[2]{};
        uint32_t barrier_count = 0;

        if (diagnostics) {
            barriers[barrier_count].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barriers[barrier_count].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barriers[barrier_count].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[barrier_count].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[barrier_count].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[barrier_count].buffer = p.buf_finals_dev.buffer;
            barriers[barrier_count].offset = 0;
            barriers[barrier_count].size = seeds.size() * 1024;
            barrier_count++;
        }

        if (need_offsets) {
            barriers[barrier_count].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barriers[barrier_count].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barriers[barrier_count].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[barrier_count].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[barrier_count].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[barrier_count].buffer = p.buf_offsets_dev.buffer;
            barriers[barrier_count].offset = 0;
            barriers[barrier_count].size = seeds.size() * 256 * sizeof(uint64_t);
            barrier_count++;
        }

        vkCmdPipelineBarrier(p.cmd_buffer,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr,
                             barrier_count, barriers,
                             0, nullptr);

        if (diagnostics) {
            VkBufferCopy copy_finals{0, 0, seeds.size() * 1024};
            vkCmdCopyBuffer(p.cmd_buffer, p.buf_finals_dev.buffer, p.buf_finals_host.buffer, 1, &copy_finals);
        }

        if (need_offsets) {
            VkBufferCopy copy_offsets{0, 0, seeds.size() * 256 * sizeof(uint64_t)};
            vkCmdCopyBuffer(p.cmd_buffer, p.buf_offsets_dev.buffer, p.buf_offsets_host.buffer, 1, &copy_offsets);
        }
    }

    VK_CHECK(vkEndCommandBuffer(p.cmd_buffer));

    if (cancelled && cancelled()) return false;

    auto start_time = std::chrono::high_resolution_clock::now();

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &p.cmd_buffer;

    VK_CHECK(vkQueueSubmit(p.queue, 1, &submit_info, p.fence));
    ++p.stats.batches_submitted;

    std::exception_ptr callback_error;
    try { if (after_enqueue) after_enqueue(); }
    catch (...) { callback_error=std::current_exception(); }

    // Wait with timeout
    VkResult wait_res = vkWaitForFences(p.device, 1, &p.fence, VK_TRUE, 5000000000ULL); // 5 sec
    if (wait_res != VK_SUCCESS) {
        throw std::runtime_error("PoM kernel timeout or error");
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    p.stats.last_dispatch_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    ++p.stats.batches_completed;
    p.stats.nonces_processed += seeds.size();
    if(callback_error)std::rethrow_exception(callback_error);

    // Read on-GPU winner atomic
    if (p.buf_winner.mapped) {
        uint32_t win = *static_cast<uint32_t*>(p.buf_winner.mapped);
        if (win != 0xFFFFFFFFu && win < seeds.size()) {
            out.winner_index = win;
        } else {
            out.winner_index = std::nullopt;
        }
    }

    if (!diagnostics && !need_offsets) {
        // Fast path: 0-byte state DMA readback!
        out.offsets.clear();
        out.final_states.clear();
        out.diagnostic_states.clear();
        return true;
    }

    // Diagnostics path: Invalidate cached host staging memory for fast CPU read
    VkMappedMemoryRange inv_ranges[2]{};
    uint32_t inv_count = 0;
    if (diagnostics && p.buf_finals_host.is_cached) {
        inv_ranges[inv_count].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        inv_ranges[inv_count].memory = p.buf_finals_host.memory;
        inv_ranges[inv_count].offset = 0;
        inv_ranges[inv_count].size = seeds.size() * 1024;
        inv_count++;
    }
    if (need_offsets && p.buf_offsets_host.is_cached) {
        inv_ranges[inv_count].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        inv_ranges[inv_count].memory = p.buf_offsets_host.memory;
        inv_ranges[inv_count].offset = 0;
        inv_ranges[inv_count].size = seeds.size() * 256 * sizeof(uint64_t);
        inv_count++;
    }
    if (inv_count > 0) {
        vkInvalidateMappedMemoryRanges(p.device, inv_count, inv_ranges);
    }

    if (need_offsets) {
        out.offsets.resize(seeds.size() * 256);
        std::memcpy(out.offsets.data(), p.buf_offsets_host.mapped, seeds.size() * 256 * sizeof(uint64_t));
    } else {
        out.offsets.clear();
    }

    if (diagnostics) {
        out.final_states.resize(seeds.size() * 1024);
        std::memcpy(out.final_states.data(), p.buf_finals_host.mapped, seeds.size() * 1024);
        out.diagnostic_states.resize(seeds.size() * 257 * 1024);
        std::memcpy(out.diagnostic_states.data(), p.buf_trace.mapped, seeds.size() * 257 * 1024);
    } else {
        out.final_states.clear();
        out.diagnostic_states.clear();
    }

    if (diagnostics) {
        out.diagnostic_states.resize(seeds.size() * 257 * 1024);
        std::memcpy(out.diagnostic_states.data(), p.buf_trace.mapped, seeds.size() * 257 * 1024);
    } else {
        out.diagnostic_states.clear();
    }

    return true;
}

void VulkanBdaResidentEngine::pause_and_drain() {
    auto &p = *impl_;
    p.paused = true;
    if (p.device && p.queue) {
        vkQueueWaitIdle(p.queue);
    }
    ++p.stats.preemptions_handled;
}

void VulkanBdaResidentEngine::resume() {
    auto &p = *impl_;
    p.paused = false;
}

bool VulkanBdaResidentEngine::is_ready() const noexcept {
    return impl_ && impl_->ready && !impl_->paused;
}

BdaEngineStats VulkanBdaResidentEngine::get_stats() const noexcept {
    return impl_ ? impl_->stats : BdaEngineStats{};
}

} // namespace keryx::nexus::vulkan
