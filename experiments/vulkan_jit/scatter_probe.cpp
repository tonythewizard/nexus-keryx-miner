// Standalone AMD Vulkan execution probe, not a miner or a production backend.
// Consumes SPIR-V generated at runtime from cache_scatter_kernel's CUDA body.
#include <vulkan/vulkan_core.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "work_replay.hpp"

static void check(VkResult result) {
    if (result != VK_SUCCESS)
        throw std::runtime_error("Vulkan error " + std::to_string(result));
}

struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped{}; };

struct Probe {
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    VkQueue queue{};
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout layout{};
    VkShaderModule shader{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    VkCommandPool commands{};
    VkFence fence{};
    std::vector<Buffer> buffers;
    bool submitted = false;
    ~Probe() {
        if (device) {
            if (submitted) vkDeviceWaitIdle(device);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (commands) vkDestroyCommandPool(device, commands, nullptr);
            if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (shader) vkDestroyShaderModule(device, shader, nullptr);
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            if (descriptors) vkDestroyDescriptorSetLayout(device, descriptors, nullptr);
            for (auto& b : buffers) {
                if (b.mapped) vkUnmapMemory(device, b.memory);
                if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
                if (b.memory) vkFreeMemory(device, b.memory, nullptr);
            }
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    void allocate(Buffer& b, VkDeviceSize size) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size; info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        check(vkCreateBuffer(device, &info, nullptr, &b.buffer));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, b.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
        const auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = 0;
        while (type < memory.memoryTypeCount &&
               (!(requirements.memoryTypeBits & (1u << type)) ||
                (memory.memoryTypes[type].propertyFlags & flags) != flags)) ++type;
        if (type == memory.memoryTypeCount) throw std::runtime_error("No coherent host memory type");
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = requirements.size; alloc.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
        check(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        check(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped));
    }
    void run(const char* filename, uint32_t tile_bytes, const char* walk_input = nullptr, const char* walk_output = nullptr, bool resident_walk = false, bool gathered_walk = false) {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        auto length = file.tellg();
        if (!file || length < 20 || length > 16 * 1024 * 1024 || length % 4)
            throw std::runtime_error("Invalid SPIR-V file size");
        std::vector<uint32_t> code(static_cast<size_t>(length) / 4);
        file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), length);
        if (!file || code.front() != 0x07230203) throw std::runtime_error("Invalid SPIR-V module");
        bool needs_dot = false;
        for (size_t i = 5; i < code.size();) {
            const auto words = code[i] >> 16;
            if (!words || i + words > code.size())
                throw std::runtime_error("Invalid SPIR-V instruction length");
            if ((code[i] & 65535u) == 17u && words == 2 && code[i + 1] == 6019u)
                needs_dot = true;
            i += words;
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance_info.pApplicationInfo = &app;
        check(vkCreateInstance(&instance_info, nullptr, &instance));
        uint32_t count = 0; check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.vendorID == 0x1002 && props.deviceID == 0x67df) {
                if (const char* requested = std::getenv("NEXUS_VULKAN_DEVICE_ID")) {
                    VkPhysicalDevicePCIBusInfoPropertiesEXT pci{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
                    VkPhysicalDeviceProperties2 identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; identity.pNext = &pci;
                    vkGetPhysicalDeviceProperties2(candidate, &identity);
                    std::ostringstream id;
                    id << "pci:" << std::hex << std::setfill('0') << std::setw(4) << pci.pciDomain << ':'
                       << std::setw(2) << pci.pciBus << ':' << std::setw(2) << pci.pciDevice << '.' << pci.pciFunction;
                    if (id.str() != requested) continue;
                    std::cout << "DEVICE_ID=" << id.str() << '\n';
                }
                if (gpu) throw std::runtime_error("Ambiguous AMD Polaris device selection");
                gpu = candidate;
                std::cout << "DEVICE=" << props.deviceName << '\n';
            }
        }
        if (!gpu) throw std::runtime_error("Authorized AMD Polaris secondary AMD GPU not found");
        uint32_t extension_count = 0;
        check(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extension_count, nullptr));
        std::vector<VkExtensionProperties> extensions(extension_count);
        check(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extension_count, extensions.data()));
        const bool has_dot = std::any_of(extensions.begin(), extensions.end(), [](const auto &e) {
            return std::strcmp(e.extensionName, VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME) == 0;
        });
        VkPhysicalDeviceShaderIntegerDotProductFeaturesKHR dot_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES_KHR};
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features12.pNext = &dot_features;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &features12;
        vkGetPhysicalDeviceFeatures2(gpu, &features);
        if (!features.features.shaderInt64 || !features12.shaderInt8 || !features12.storageBuffer8BitAccess ||
            (needs_dot && (!has_dot || !dot_features.shaderIntegerDotProduct)))
            throw std::runtime_error("Required integer/storage features unavailable");
        uint32_t families = 0; vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, queues.data());
        uint32_t family = 0;
        while (family < families && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
        if (family == families) throw std::runtime_error("No compute queue");
        float priority = 1;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family; queue_info.queueCount = 1; queue_info.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures enabled{}; enabled.shaderInt64 = VK_TRUE;
        features12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        dot_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES_KHR};
        dot_features.shaderIntegerDotProduct = VK_TRUE;
        features12.pNext = needs_dot ? &dot_features : nullptr;
        features12.shaderInt8 = VK_TRUE; features12.storageBuffer8BitAccess = VK_TRUE;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        const char *dot_extension = VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME;
        info.enabledExtensionCount = needs_dot ? 1 : 0;
        info.ppEnabledExtensionNames = needs_dot ? &dot_extension : nullptr;
        info.pNext = &features12; info.pEnabledFeatures = &enabled;
        info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queue_info;
        check(vkCreateDevice(gpu, &info, nullptr, &device)); vkGetDeviceQueue(device, family, 0, &queue);
        constexpr uint32_t tiles = 32;
        const size_t bytes = size_t(tiles) * tile_bytes;
        uint32_t groups = tiles + 2;
        std::vector<uint8_t> push_data(4);
        std::memcpy(push_data.data(), &tiles, 4);
        std::vector<std::vector<uint8_t>> host;
        std::vector<uint8_t> expected(bytes + 3 * tile_bytes, 0xcd);
        if (!walk_input) {
            host = {expected, std::vector<uint8_t>(bytes), std::vector<uint8_t>(tiles * 4)};
            for (size_t i = 0; i < bytes; ++i) host[1][i] = uint8_t((i * 37 + (i >> 8) * 13) & 255);
            for (uint32_t t = 0; t < tiles; ++t) {
                uint32_t slot = tiles - 1 - t;
                std::memcpy(host[2].data() + t * 4, &slot, 4);
                std::memcpy(expected.data() + slot * tile_bytes, host[1].data() + t * tile_bytes, tile_bytes);
            }
        } else {
            std::ifstream fixture(walk_input, std::ios::binary);
            char magic[8]{}; uint32_t nonces{}, diagnostics{}; uint64_t ntiles{}; uint32_t pool_tiles{};
            fixture.read(magic, 8);
            fixture.read(reinterpret_cast<char*>(&nonces), 4);
            fixture.read(reinterpret_cast<char*>(&diagnostics), 4);
            fixture.read(reinterpret_cast<char*>(&ntiles), 8);
            if (gathered_walk) fixture.read(reinterpret_cast<char*>(&pool_tiles), 4);
            else if (ntiles <= 1024) pool_tiles = static_cast<uint32_t>(ntiles);
            if (!fixture || std::memcmp(magic, gathered_walk ? "NXWG1\0\0\0" : resident_walk ? "NXWR1\0\0\0" : "NXWK1\0\0\0", 8) || nonces < 1 || nonces > 4 || ntiles < 1 || (!gathered_walk && ntiles > 1024) || ntiles > (1ULL << 32) || pool_tiles < 1 || pool_tiles > 1024 || diagnostics > 1)
                throw std::runtime_error("Invalid bounded walk fixture");
            groups = nonces; push_data.resize(12);
            std::memcpy(push_data.data(), &ntiles, 8); std::memcpy(push_data.data() + 8, &diagnostics, 4);
            host = {std::vector<uint8_t>(nonces * 8), std::vector<uint8_t>(pool_tiles * 1024),
                    std::vector<uint8_t>(nonces * 256 * 4), std::vector<uint8_t>(nonces * 256 * 8 + 16, 0xcd),
                    std::vector<uint8_t>(nonces * 1024 + 16, 0xcd),
                    std::vector<uint8_t>((diagnostics ? nonces * 257 * 1024 : 0) + 16, 0xcd)};
            if (resident_walk) host.erase(host.begin() + 2);
            for (size_t i = 0; i < (resident_walk ? 2u : 3u); ++i) fixture.read(reinterpret_cast<char*>(host[i].data()), host[i].size());
            if (!fixture || fixture.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Truncated/extra walk fixture data");
            for (size_t i = 0; !resident_walk && i < host[2].size(); i += 4) {
                uint32_t slot{}; std::memcpy(&slot, host[2].data() + i, 4);
                if (slot >= pool_tiles) throw std::runtime_error("Tile slot outside bounded buffer");
            }
        }
        std::unique_ptr<WorkReplay> work_replay;
        if (const char* context = std::getenv("NEXUS_VULKAN_REPLAY_CONTEXT")) {
            if (!gathered_walk || groups != 1) throw std::runtime_error("Work replay requires one gathered nonce");
            uint64_t seed{}; std::memcpy(&seed, host[0].data(), 8);
            work_replay = std::make_unique<WorkReplay>(context, std::getenv("NEXUS_VULKAN_REPLAY_EVENT"), seed);
        }
        buffers.resize(host.size());
        for (size_t i = 0; i < host.size(); ++i) {
            allocate(buffers[i], host[i].size()); std::memcpy(buffers[i].mapped, host[i].data(), host[i].size());
        }
        const auto binding_count = static_cast<uint32_t>(buffers.size());
        std::vector<VkDescriptorSetLayoutBinding> bindings(binding_count);
        for (uint32_t i = 0; i < binding_count; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo desc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        desc.bindingCount = binding_count; desc.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &desc, nullptr, &descriptors));
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, static_cast<uint32_t>(push_data.size())};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.setLayoutCount = 1; layout_info.pSetLayouts = &descriptors;
        layout_info.pushConstantRangeCount = 1; layout_info.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout));
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_info.codeSize = code.size() * 4; shader_info.pCode = code.data();
        check(vkCreateShaderModule(device, &shader_info, nullptr, &shader));
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline_info.stage.module = shader;
        pipeline_info.stage.pName = "main"; pipeline_info.layout = layout;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, binding_count};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = 1; pool_info.poolSizeCount = 1; pool_info.pPoolSizes = &pool_size;
        check(vkCreateDescriptorPool(device, &pool_info, nullptr, &pool));
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_info.descriptorPool = pool; set_info.descriptorSetCount = 1; set_info.pSetLayouts = &descriptors;
        VkDescriptorSet set{}; check(vkAllocateDescriptorSets(device, &set_info, &set));
        std::vector<VkDescriptorBufferInfo> buffer_info(binding_count);
        std::vector<VkWriteDescriptorSet> writes(binding_count);
        for (uint32_t i = 0; i < binding_count; ++i) {
            buffer_info[i] = {buffers[i].buffer, 0, VK_WHOLE_SIZE};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet = set;
            writes[i].dstBinding = i; writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buffer_info[i];
        }
        vkUpdateDescriptorSets(device, binding_count, writes.data(), 0, nullptr);
        VkCommandPoolCreateInfo command_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; command_pool.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &command_pool, nullptr, &commands));
        VkCommandBufferAllocateInfo command_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_alloc.commandPool = commands; command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; command_alloc.commandBufferCount = 1;
        VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device, &command_alloc, &command));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(command, &begin));
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, static_cast<uint32_t>(push_data.size()), push_data.data());
        vkCmdDispatch(command, groups, 1, 1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(command));
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(device, &fence_info, nullptr, &fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        auto started = std::chrono::steady_clock::now();
        if (work_replay) work_replay->before_dispatch();
        check(vkQueueSubmit(queue, 1, &submit, fence)); submitted = true;
        if (work_replay) work_replay->after_enqueue();
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ULL)); submitted = false;
        if (work_replay && !work_replay->after_fence()) return;
        if (!walk_input) {
            if (std::memcmp(expected.data(), buffers[0].mapped, expected.size())) throw std::runtime_error("Byte parity or guard failed");
            std::cout << "SCATTER_BYTE_PARITY=PASS\nGUARD_BYTES=PASS\nCOPIED_BYTES=" << bytes << '\n';
        } else {
            std::ofstream output(walk_output, std::ios::binary);
            output.write("NXWO1\0\0\0", 8);
            for (size_t i = resident_walk ? 2u : 3u; i < host.size(); ++i) output.write(static_cast<const char*>(buffers[i].mapped), host[i].size());
            if (!output) throw std::runtime_error("Cannot persist GPU results");
            std::cout << "WALK_DISPATCH_COMPLETED=YES\nWALK_NONCES=" << groups << '\n';
        }
        std::cout << "DISPATCH_AND_WAIT_MS=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() << '\n';
    }
};

int main(int argc, char** argv) {
    try {
        if (argc != 3 && argc != 5) throw std::runtime_error("Usage: scatter-probe generated.spv tile_bytes | generated.spv --walk INPUT OUTPUT");
        // Limit loader discovery to AMD; never enumerate an NVIDIA device.
        setenv("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/radeon_icd.json", 1);
        setenv("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/radeon_icd.json", 1);
        setenv("VK_LOADER_LAYERS_DISABLE", "~implicit~", 1);
        if (argc == 5) {
            const std::string mode(argv[2]);
            if (mode != "--walk" && mode != "--resident-walk" && mode != "--gathered-walk") throw std::runtime_error("Invalid walk mode");
            Probe probe; probe.run(argv[1], 1024, argv[3], argv[4], mode == "--resident-walk", mode == "--gathered-walk"); return 0;
        }
        std::string argument(argv[2]); size_t used = 0;
        auto tile = std::stoul(argument, &used);
        if (used != argument.size() || tile < 1 || tile > 4096) throw std::runtime_error("Invalid tile size");
        Probe probe; probe.run(argv[1], static_cast<uint32_t>(tile));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BLOCKED: " << error.what() << '\n'; return 1;
    }
}
