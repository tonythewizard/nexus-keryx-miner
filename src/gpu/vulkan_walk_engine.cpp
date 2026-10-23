#include "keryx/nexus/developer_config.hpp"
// Persistent bounded Vulkan host engine; CUDA source is translated separately.
#include "keryx/nexus/consensus/vulkan_engine.hpp"
#include "keryx/nexus/vulkan_device_policy.hpp"
#include "keryx/nexus/vulkan_route_capability.hpp"
#include "keryx/nexus/consensus/pom_v4_bda_spv.hpp"
#include "keryx/nexus/consensus/vulkan_bda_resident_engine.hpp"
#include <vulkan/vulkan_core.h>
#include <openssl/evp.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <regex>
#include <stdexcept>
namespace keryx::nexus::consensus {
namespace {
void check(VkResult result){if(result!=VK_SUCCESS)throw std::runtime_error("Vulkan error "+std::to_string(result));}
struct Buffer { VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};VkDeviceSize bytes{}; };
}
struct VulkanWalkEngine::Impl {
    mutable std::mutex mutex;
    VulkanEngineStats counters;
    size_t capacity{};
    std::uint64_t resident_tiles{}, split_tiles{}, uploaded_tiles{};
    bool resident_loaded{};
    VkQueryPool timing_pool{};
    float timestamp_period{};
    bool cached_readback{true};
    bool supports_offset_suppression{};
    Buffer staging;
    VkDeviceSize storage_limit{}, allocation_limit{};
    bool failed{};
    VkCommandBuffer command{};
    VkDescriptorSet set{};
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queue_family{};
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout layout{};
    VkShaderModule shader{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    VkCommandPool commands{};
    VkFence fence{};
    std::vector<Buffer> buffers;
    bool submitted = false;
    bool is_bda{false};
    std::unique_ptr<vulkan::VulkanBdaResidentEngine> bda_engine;
    std::vector<Buffer> bda_model_buffers;
    ~Impl() {
        if (device) {
            if (submitted) vkDeviceWaitIdle(device);
            bda_engine.reset();
            for (auto& b : bda_model_buffers) {
                if (b.mapped) vkUnmapMemory(device, b.memory);
                if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
                if (b.memory) vkFreeMemory(device, b.memory, nullptr);
            }
            bda_model_buffers.clear();
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (commands) vkDestroyCommandPool(device, commands, nullptr);
            if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (shader) vkDestroyShaderModule(device, shader, nullptr);
            if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
            if (descriptors) vkDestroyDescriptorSetLayout(device, descriptors, nullptr);
            if(staging.mapped)vkUnmapMemory(device,staging.memory);
            if(staging.buffer)vkDestroyBuffer(device,staging.buffer,nullptr);
            if(staging.memory)vkFreeMemory(device,staging.memory,nullptr);
            for (auto& b : buffers) {
                if (b.mapped) vkUnmapMemory(device, b.memory);
                if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
                if (b.memory) vkFreeMemory(device, b.memory, nullptr);
            }
            if(timing_pool)vkDestroyQueryPool(device,timing_pool,nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    void allocate(Buffer& b, VkDeviceSize size, bool local=false, bool upload=false,bool cpu_read=false,bool bda=false) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = upload?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (local?VK_BUFFER_USAGE_TRANSFER_DST_BIT:0) | (bda?VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT:0);
        if(!upload && size>storage_limit)throw std::runtime_error("Storage buffer exceeds queried range");
        check(vkCreateBuffer(device, &info, nullptr, &b.buffer));
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, b.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
        if(requirements.size>allocation_limit)throw std::runtime_error("Buffer exceeds queried maximum allocation");
        const auto flags = local?VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT:
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = 0;
        while (type < memory.memoryTypeCount &&
               (!(requirements.memoryTypeBits & (1u << type)) ||
                (memory.memoryTypes[type].propertyFlags & flags) != flags)) ++type;
        if(cpu_read&&cached_readback) {
            const auto preferred=flags|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            for(uint32_t cached=0;cached<memory.memoryTypeCount;++cached)
                if((requirements.memoryTypeBits&(1u<<cached))&&
                   (memory.memoryTypes[cached].propertyFlags&preferred)==preferred){type=cached;break;}
        }
        if (type == memory.memoryTypeCount) throw std::runtime_error("No coherent host memory type");
        if(cpu_read)std::cout<<"VULKAN_READBACK_MEMORY cached="
            <<((memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)?"true":"false")
            <<" coherent=true type="<<type<<" bytes="<<size<<"\n";
        VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        if(bda) flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.pNext = bda ? &flags_info : nullptr;
        alloc.allocationSize = requirements.size; alloc.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &alloc, nullptr, &b.memory));
        b.bytes=requirements.size;counters.allocated_bytes += requirements.size;
        check(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        if(!local)check(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped));
    }
    void release(Buffer &b) noexcept {
        if(b.mapped)vkUnmapMemory(device,b.memory);
        if(b.buffer)vkDestroyBuffer(device,b.buffer,nullptr);
        if(b.memory)vkFreeMemory(device,b.memory,nullptr);
        counters.allocated_bytes-=b.bytes;b={};
    }
    std::size_t diagnostic_capacity{1};
    void reserve_diagnostics(std::size_t count) {
        if(count<=diagnostic_capacity)return;
        // Previous dispatches have drained under the engine mutex. Drop their
        // recorded buffer references before replacing only the trace buffer.
        check(vkResetCommandPool(device,commands,0));
        release(buffers[5]);
        allocate(buffers[5],count*257*1024+16,false,false,true);
        VkDescriptorBufferInfo info{buffers[5].buffer,0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=set;write.dstBinding=5;write.descriptorCount=1;
        write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&info;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        diagnostic_capacity=count;
    }
    void bind_model_banks() {
        std::array<VkDescriptorBufferInfo,2> info{};
        std::array<VkWriteDescriptorSet,2> writes{};
        for(unsigned i=0;i<2;++i){
            info[i]={buffers[i+1].buffer,0,VK_WHOLE_SIZE};
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;
            writes[i].dstBinding=i+1;writes[i].descriptorCount=1;
            writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&info[i];
        }
        vkUpdateDescriptorSets(device,2,writes.data(),0,nullptr);
    }
    void initialize(const std::string &pci_id,const char* filename,const std::string &expected_sha,size_t cap,std::uint64_t resident,bool cache_outputs) {
        const char* icd=std::getenv("VK_DRIVER_FILES");
        const char* legacy=std::getenv("VK_ICD_FILENAMES");
        const char* layers=std::getenv("VK_LOADER_LAYERS_DISABLE");
        const bool nvidia_test=pci_id=="" NEXUS_PRIMARY_GPU_UUID "";
        const char* expected_icd=nvidia_test?"/usr/share/vulkan/icd.d/nvidia_icd.json":"/usr/share/vulkan/icd.d/radeon_icd.json";
        const bool nvidia_selected=nvidia_vulkan_selected();
        if(nvidia_test && !nvidia_selected)
            throw std::runtime_error("NVIDIA Vulkan replay requires explicit authorized test UUID");
        const bool mixed=std::getenv("NEXUS_INDEPENDENT_VULKAN_LANES") &&
            std::string_view(std::getenv("NEXUS_INDEPENDENT_VULKAN_LANES"))=="1";
        const std::string allowed_mixed="/usr/share/vulkan/icd.d/nvidia_icd.json:/usr/share/vulkan/icd.d/radeon_icd.json";
        if(!icd || (std::string(icd)!=expected_icd && !(mixed && icd==allowed_mixed)) ||
           !legacy || std::string(legacy)!=icd || !layers || std::string(layers)!="~implicit~")
            throw std::runtime_error("Explicit device-scoped Vulkan loader environment is required");
        if((!nvidia_test && pci_id!="" NEXUS_SECONDARY_GPU_ID "") || cap<1 || cap>32768)throw std::runtime_error("Unsupported device/capacity");
        capacity=cap;
        cached_readback=cache_outputs;
        if(resident && (resident<2 || resident>(1ULL<<32)))throw std::invalid_argument("Invalid resident canonical tile count");
        resident_tiles=resident;split_tiles=(resident+1)/2;
        const bool bda_mode = (std::string_view(filename) == "bda-native");
        this->is_bda = bda_mode;
        std::vector<uint32_t> code;
        bool needs_dot=false;
        if(bda_mode) {
            code.assign(pom_v4_bda_resident_spv, pom_v4_bda_resident_spv + sizeof(pom_v4_bda_resident_spv)/sizeof(uint32_t));
            supports_offset_suppression = true;
        } else {
            std::ifstream file(filename, std::ios::binary | std::ios::ate);
            auto length = file.tellg();
            if (!file || length < 20 || length > 16 * 1024 * 1024 || length % 4)
                throw std::runtime_error("Invalid SPIR-V file size");
            code.resize(static_cast<size_t>(length) / 4);
            file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), length);
            if (!file || code.front() != 0x07230203) throw std::runtime_error("Invalid SPIR-V module");
            for(size_t i=5;i<code.size();) {
                const auto words=code[i]>>16;
                if(!words||i+words>code.size())throw std::runtime_error("Invalid SPIR-V instruction length");
                if((code[i]&65535u)==17u&&words==2&&code[i+1]==6019u)needs_dot=true;
                i+=words;
            }
            unsigned char digest[32];unsigned size=0;
            if(EVP_Digest(code.data(),code.size()*4,digest,&size,EVP_sha256(),nullptr)!=1 || size!=32)
                throw std::runtime_error("Module digest failed");
            std::ostringstream hash;
            for(auto b:digest)hash<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(b);
            if(hash.str()!=expected_sha)throw std::runtime_error("JIT module SHA-256 mismatch");
            if(resident_tiles) {
                std::ifstream manifest(std::filesystem::path(filename).parent_path()/"manifest.json",std::ios::binary|std::ios::ate);
                if(!manifest||manifest.tellg()>65536)throw std::runtime_error("Resident JIT manifest missing or oversized");
                manifest.seekg(0);const std::string metadata((std::istreambuf_iterator<char>(manifest)),{});
                supports_offset_suppression = std::regex_search(metadata,std::regex(R"("offset_suppression"\s*:\s*true)"));
                if(!std::regex_search(metadata,std::regex(R"("entry"\s*:\s*"pom_v4_banked_resident_walk_kernel")")) ||
                   !std::regex_search(metadata,std::regex("\"spirv_sha256\"\\s*:\\s*\""+expected_sha+"\"")))
                    throw std::runtime_error("Resident module manifest does not match banked ABI/hash");
            }
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance_info.pApplicationInfo = &app;
        check(vkCreateInstance(&instance_info, nullptr, &instance));
        uint32_t count = 0; check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        // The test ICD must expose one adapter: never inspect an excluded GPU.
        if(nvidia_test && !mixed && count!=1)throw std::runtime_error("NVIDIA replay requires exactly one visible Vulkan device");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(candidate, &props);
            if(nvidia_test) {
                if(props.vendorID!=0x10de)continue;
                VkPhysicalDeviceIDProperties uuid{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};identity.pNext=&uuid;
                vkGetPhysicalDeviceProperties2(candidate,&identity);
                std::ostringstream id;id<<"GPU-"<<std::hex<<std::setfill('0');
                for(unsigned i=0;i<VK_UUID_SIZE;++i){if(i==4||i==6||i==8||i==10)id<<'-';id<<std::setw(2)<<unsigned(uuid.deviceUUID[i]);}
                if(props.vendorID!=0x10de || props.deviceID!=0x2482 || id.str()!=pci_id)
                    throw std::runtime_error("NVIDIA replay UUID/PCI identity mismatch");
                if(gpu)throw std::runtime_error("Ambiguous NVIDIA Vulkan device selection");
                gpu=candidate;std::cout<<"DEVICE_ID="<<id.str()<<"\nDEVICE="<<props.deviceName<<'\n';
            } else if (props.vendorID == 0x1002 && props.deviceID == 0x67df) {
                {
                    const char* requested = pci_id.c_str();
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
        if (!gpu) throw std::runtime_error("Authorized Vulkan device not found");
        if(bda_mode)require_bda_matrix(instance,gpu);
        VkPhysicalDeviceShaderIntegerDotProductPropertiesKHR dot_props{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES_KHR};
        VkPhysicalDeviceMaintenance3Properties maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
        maintenance.pNext=&dot_props;
        VkPhysicalDeviceProperties2 limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};limits.pNext=&maintenance;
        vkGetPhysicalDeviceProperties2(gpu,&limits);
        storage_limit=limits.properties.limits.maxStorageBufferRange;
        allocation_limit=maintenance.maxMemoryAllocationSize;
        timestamp_period=limits.properties.limits.timestampPeriod;
        std::uint32_t extension_count=0;check(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extension_count,nullptr));
        std::vector<VkExtensionProperties> extensions(extension_count);
        check(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extension_count,extensions.data()));
        const bool has_dot_ext=std::any_of(extensions.begin(),extensions.end(),[](const auto &x){
            return std::strcmp(x.extensionName,VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME)==0;
        });
        if(resident_tiles) {
            if(std::none_of(extensions.begin(),extensions.end(),[](const auto &x){return std::strcmp(x.extensionName,VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)==0;}))
                throw std::runtime_error("Resident admission requires queried Vulkan memory budget");
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
            VkPhysicalDeviceMemoryProperties2 memory{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};memory.pNext=&budget;
            vkGetPhysicalDeviceMemoryProperties2(gpu,&memory);
            VkDeviceSize largest=0;
            for(unsigned i=0;i<memory.memoryProperties.memoryHeapCount;++i)
                if(memory.memoryProperties.memoryHeaps[i].flags&VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    largest=std::max(largest,budget.heapBudget[i]>budget.heapUsage[i]?budget.heapBudget[i]-budget.heapUsage[i]:0);
            if(resident_tiles*1024+128ULL*1024*1024>largest)
                throw std::runtime_error("Resident model leaves insufficient device-local heap reserve");
            if(split_tiles*1024>std::min<VkDeviceSize>(storage_limit,0xffffffffULL))
                throw std::runtime_error("Canonical model requires more than two supported storage banks");
        }
        VkPhysicalDeviceShaderIntegerDotProductFeaturesKHR dot_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES_KHR};
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features12.pNext=&dot_features;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &features12;
        vkGetPhysicalDeviceFeatures2(gpu, &features);
        if (!features.features.shaderInt64 || !features12.shaderInt8 || !features12.storageBuffer8BitAccess ||
            (needs_dot && (!has_dot_ext || !dot_features.shaderIntegerDotProduct)))
            throw std::runtime_error("Required integer/storage/dot-product features unavailable");
        std::cout << "VULKAN_INT8_DOT_PRODUCT supported="<<(has_dot_ext && dot_features.shaderIntegerDotProduct ? "true" : "false")<<" accelerated_4x8_packed="
                  << (dot_props.integerDotProduct4x8BitPackedSignedAccelerated ? "true" : "false") << "\n";
        uint32_t families = 0; vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, queues.data());
        uint32_t family = 0;
        while (family < families && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
        if (family == families) throw std::runtime_error("No compute queue");
        queue_family=family;
        float priority = 1;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family; queue_info.queueCount = 1; queue_info.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures enabled{}; enabled.shaderInt64 = VK_TRUE; enabled.shaderInt16 = VK_TRUE;
        dot_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES_KHR};
        dot_features.shaderIntegerDotProduct = VK_TRUE;
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop_features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, nullptr, VK_TRUE, VK_FALSE
        };
        VkPhysicalDevice16BitStorageFeatures features16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
        features16.storageBuffer16BitAccess = VK_TRUE;
        features16.pNext = &coop_features;
        features12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features12.pNext = bda_mode ? (void*)&features16 : (needs_dot ? (void*)&dot_features : nullptr);
        features12.shaderInt8 = VK_TRUE; features12.storageBuffer8BitAccess = VK_TRUE;
        if(bda_mode) features12.bufferDeviceAddress = VK_TRUE;
        const char* enabled_exts[] = {
            VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME,
            VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME
        };
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pNext = &features12; info.pEnabledFeatures = &enabled;
        info.enabledExtensionCount = bda_mode ? 1 : (needs_dot ? 1 : 0);
        info.ppEnabledExtensionNames = bda_mode ? &enabled_exts[1] : (needs_dot ? &enabled_exts[0] : nullptr);
        info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queue_info;
        check(vkCreateDevice(gpu, &info, nullptr, &device)); vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo command_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; command_pool.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &command_pool, nullptr, &commands));
        VkCommandBufferAllocateInfo command_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_alloc.commandPool = commands; command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; command_alloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &command_alloc, &command));
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device,&fence_info,nullptr,&fence));
        buffers.resize(3);
        if(bda_mode) {
            bda_engine = std::make_unique<vulkan::VulkanBdaResidentEngine>(device, gpu, family, queue, code);
            return;
        }
        if(std::getenv("NEXUS_VULKAN_BENCH_TIMING")) {
            if(!queues[family].timestampValidBits)throw std::runtime_error("Benchmark queue has no timestamps");
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;
            check(vkCreateQueryPool(device,&qi,nullptr,&timing_pool));
        }
        const std::array<size_t,6> sizes{capacity*8,
            resident_tiles?split_tiles*1024:capacity*256*1024,
            resident_tiles?(resident_tiles-split_tiles)*1024:capacity*256*4,
            capacity*256*8+16,capacity*1024+16,diagnostic_capacity*257*1024+16};
        buffers.resize(sizes.size());
        for (size_t i = 0; i < sizes.size(); ++i) {
            allocate(buffers[i], sizes[i],resident_tiles&&(i==1||i==2),false,i>=3);
        }
        const auto binding_count = static_cast<uint32_t>(buffers.size());
        std::vector<VkDescriptorSetLayoutBinding> bindings(binding_count);
        for (uint32_t i = 0; i < binding_count; ++i) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo desc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        desc.bindingCount = binding_count; desc.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &desc, nullptr, &descriptors));
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, resident_tiles?20u:12u};
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
        check(vkAllocateDescriptorSets(device, &set_info, &set));
        std::vector<VkDescriptorBufferInfo> buffer_info(binding_count);
        std::vector<VkWriteDescriptorSet> writes(binding_count);
        for (uint32_t i = 0; i < binding_count; ++i) {
            buffer_info[i] = {buffers[i].buffer, 0, VK_WHOLE_SIZE};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet = set;
            writes[i].dstBinding = i; writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buffer_info[i];
        }
        vkUpdateDescriptorSets(device, binding_count, writes.data(), 0, nullptr);
    }
};
VulkanWalkEngine::VulkanWalkEngine(const std::string &pci,const std::filesystem::path &module,
    const std::string &sha,std::size_t capacity,std::uint64_t resident_tiles,bool cached_readback):impl_(std::make_unique<Impl>()) {
    impl_->initialize(pci,module.c_str(),sha,capacity,resident_tiles,cached_readback);
}
VulkanWalkEngine::~VulkanWalkEngine()=default;
bool VulkanWalkEngine::screens_target_on_gpu() const noexcept { return impl_->is_bda; }
VulkanEngineStats VulkanWalkEngine::stats() const {
    std::lock_guard lock(impl_->mutex);
    if(impl_->is_bda && impl_->bda_engine) {
        auto s = impl_->bda_engine->get_stats();
        VulkanEngineStats res = impl_->counters;
        res.submitted = s.batches_submitted;
        res.completed = s.batches_completed;
        return res;
    }
    return impl_->counters;
}
bool VulkanWalkEngine::resident() const {return impl_->resident_tiles!=0;}
std::uint64_t VulkanWalkEngine::resident_split_tiles() const {return impl_->split_tiles;}
void VulkanWalkEngine::release_resident() {
    auto &p=*impl_;std::lock_guard lock(p.mutex);
    if(p.is_bda) {
        p.bda_engine.reset(); // drains before model addresses can be freed
        p.release(p.buffers[1]);
        p.release(p.buffers[2]);
        p.resident_loaded=false;
        p.uploaded_tiles=0;
        return;
    }
    if(!p.resident_tiles||p.failed||p.submitted)
        throw std::logic_error("Resident banks cannot be released in this engine state");
    // All submissions drain before releasing the mutex. Reset recorded commands
    // before destroying their resources; descriptors are rewritten before reuse.
    check(vkResetCommandPool(p.device,p.commands,0));
    p.release(p.staging);p.release(p.buffers[1]);p.release(p.buffers[2]);
    p.uploaded_tiles=0;p.resident_loaded=false;
}
bool VulkanWalkEngine::load_resident(
    const std::function<void(std::uint64_t,std::span<std::uint8_t>)> &read,
    const std::function<bool()> &cancelled) {
    auto &p=*impl_;std::lock_guard lock(p.mutex);
    if(p.resident_loaded) return true;
    if(!p.resident_tiles||p.failed||!read)throw std::logic_error("Invalid resident upload lifecycle");
    constexpr std::uint64_t staging_bytes=4ULL*1024*1024;
    try {
        if(cancelled&&cancelled())return false;
        if(p.buffers.size()<3) p.buffers.resize(3);
        if(!p.buffers[1].buffer)p.allocate(p.buffers[1],p.split_tiles*1024,true,false,false,p.is_bda);
        if(!p.buffers[2].buffer)p.allocate(p.buffers[2],(p.resident_tiles-p.split_tiles)*1024,true,false,false,p.is_bda);
        if(!p.is_bda) p.bind_model_banks();
        if(!p.staging.buffer)p.allocate(p.staging,staging_bytes,false,true);
        while(p.uploaded_tiles<p.resident_tiles) {
                if(cancelled&&cancelled())return false;
                const unsigned bank=p.uploaded_tiles>=p.split_tiles?1:0;
                const auto first=bank?p.split_tiles:0;
                const auto end=bank?p.resident_tiles:p.split_tiles;
                const auto done=p.uploaded_tiles-first;
                const auto take=std::min<std::uint64_t>(end-p.uploaded_tiles,staging_bytes/1024);
                read(p.uploaded_tiles,{static_cast<std::uint8_t*>(p.staging.mapped),static_cast<size_t>(take*1024)});
                if(cancelled&&cancelled())return false;
                check(vkResetCommandPool(p.device,p.commands,0));check(vkResetFences(p.device,1,&p.fence));
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};check(vkBeginCommandBuffer(p.command,&begin));
                VkBufferCopy copy{0,done*1024,take*1024};
                vkCmdCopyBuffer(p.command,p.staging.buffer,p.buffers[bank+1].buffer,1,&copy);
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(p.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
                check(vkEndCommandBuffer(p.command));
                VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&p.command;
                check(vkQueueSubmit(p.queue,1,&info,p.fence));p.submitted=true;
                check(vkWaitForFences(p.device,1,&p.fence,VK_TRUE,UINT64_MAX));p.submitted=false;
                p.uploaded_tiles+=take;
        }
        p.resident_loaded=true;
        vkUnmapMemory(p.device,p.staging.memory);p.staging.mapped=nullptr;
        vkDestroyBuffer(p.device,p.staging.buffer,nullptr);vkFreeMemory(p.device,p.staging.memory,nullptr);
        p.counters.allocated_bytes-=p.staging.bytes;p.staging={};
        if(p.is_bda) {
            if(!p.bda_engine) p.bda_engine=std::make_unique<vulkan::VulkanBdaResidentEngine>(
                p.device,p.gpu,p.queue_family,p.queue,
                std::vector<uint32_t>(std::begin(pom_v4_bda_resident_spv),std::end(pom_v4_bda_resident_spv)));
            VkBufferDeviceAddressInfo addr1_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            addr1_info.buffer = p.buffers[1].buffer;
            VkDeviceAddress addr1 = vkGetBufferDeviceAddress(p.device, &addr1_info);

            VkBufferDeviceAddressInfo addr2_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            addr2_info.buffer = p.buffers[2].buffer;
            VkDeviceAddress addr2 = vkGetBufferDeviceAddress(p.device, &addr2_info);

            std::vector<vulkan::BdaTensorDescriptor> descriptors = {
                {"model_bank_0", addr1, p.split_tiles * 1024, p.split_tiles * 32},
                {"model_bank_1", addr2, (p.resident_tiles - p.split_tiles) * 1024, (p.resident_tiles - p.split_tiles) * 32}
            };
            p.bda_engine->attach_resident_tensors(descriptors, p.resident_tiles);
        }
        return true;
    }catch(...){p.failed=true;throw;}
}
bool VulkanWalkEngine::run(std::uint64_t logical_tiles,std::span<const std::uint64_t> seeds,
    std::span<const std::uint8_t> tiles,std::span<const std::uint32_t> slots,bool diagnostics,
    VulkanWalkBatch &out,const std::function<bool()> &cancelled,const std::function<void()> &after_enqueue,
    bool copy_offsets,
    const std::uint64_t *words, const std::uint64_t *targets) {
    auto &p=*impl_;std::lock_guard lock(p.mutex);
    out.offsets.clear();out.final_states.clear();out.diagnostic_states.clear();
    out.winner_index = std::nullopt;
    if(p.failed)throw std::runtime_error("Vulkan engine is failed; automatic reuse prohibited");
    if(p.is_bda) {
        if(!p.bda_engine || !p.resident_loaded) throw std::runtime_error("BDA resident engine not ready");
        if(logical_tiles!=p.resident_tiles || seeds.empty() || seeds.size()>p.capacity ||
           !tiles.empty() || !slots.empty())
            throw std::invalid_argument("Invalid BDA resident batch dimensions");
        const auto is_cancelled=[&]{return cancelled && cancelled();};
        if(is_cancelled()){++p.counters.cancelled_before_dispatch;return false;}
        vulkan::BdaWalkBatch bda_batch;
        const bool ok = p.bda_engine->run_batch(seeds, diagnostics, copy_offsets, bda_batch,
                                                words ? words[0] : 0, words ? words[1] : 0, words ? words[2] : 0, words ? words[3] : 0,
                                                targets ? targets[0] : 0, targets ? targets[1] : 0, targets ? targets[2] : 0, targets ? targets[3] : 0,
                                                cancelled, after_enqueue);
        if(!ok) {
            if(is_cancelled()) ++p.counters.cancelled_before_dispatch;
            return false;
        }
        out.offsets = std::move(bda_batch.offsets);
        out.final_states = std::move(bda_batch.final_states);
        out.diagnostic_states = std::move(bda_batch.diagnostic_states);
        out.winner_index = bda_batch.winner_index;
        p.counters.completed += seeds.size();
        p.counters.submitted += seeds.size();
        if(is_cancelled()) {
            ++p.counters.discarded_after_completion;
            out={};
            return false;
        }
        return true;
    }
    if(logical_tiles<1 || logical_tiles>(1ULL<<32) || seeds.empty() || seeds.size()>p.capacity ||
       (!p.resident_tiles&&(tiles.empty() || tiles.size()%1024 || tiles.size()>p.capacity*256*1024 || slots.size()!=seeds.size()*256)) ||
       (p.resident_tiles&&(!p.resident_loaded||logical_tiles!=p.resident_tiles||!tiles.empty()||!slots.empty())))
        throw std::invalid_argument("Invalid bounded Vulkan walk dimensions");
    for(auto slot:slots)if(slot>=tiles.size()/1024)throw std::invalid_argument("Vulkan slot outside uploaded pool");
    const auto is_cancelled=[&]{return cancelled && cancelled();};
    if(is_cancelled()){++p.counters.cancelled_before_dispatch;return false;}
    try {
        if(diagnostics)p.reserve_diagnostics(seeds.size());
        const bool write_offsets = copy_offsets || diagnostics || !p.supports_offset_suppression || std::getenv("NEXUS_BENCH_FORCE_OFFSETS_WRITE");
        const std::array<size_t,3> lengths{seeds.size()*256*8,seeds.size()*1024,diagnostics?seeds.size()*257*1024:0};
        const std::array<size_t,3> capacities{p.capacity*256*8+16,p.capacity*1024+16,p.diagnostic_capacity*257*1024+16};
        auto guard_start=lengths;
        if(!write_offsets)guard_start[0]=capacities[0]-16;
        const size_t check_count = diagnostics ? 3 : 2;
        for(size_t i=0;i<check_count;++i)std::memset(static_cast<uint8_t*>(p.buffers[i+3].mapped)+guard_start[i],0xcd,capacities[i]-guard_start[i]);
        std::memcpy(p.buffers[0].mapped,seeds.data(),seeds.size_bytes());
        if(!p.resident_tiles) {
            std::memcpy(p.buffers[1].mapped,tiles.data(),tiles.size_bytes());
            std::memcpy(p.buffers[2].mapped,slots.data(),slots.size_bytes());
        }
        if(is_cancelled()){++p.counters.cancelled_before_dispatch;return false;}
        std::array<std::uint8_t,20> push{};const std::uint32_t diagnostic_flag=diagnostics?1u:(write_offsets?0u:2u);
        std::memcpy(push.data(),&logical_tiles,8);
        if(p.resident_tiles)std::memcpy(push.data()+8,&p.split_tiles,8);
        std::memcpy(push.data()+(p.resident_tiles?16:8),&diagnostic_flag,4);
        check(vkResetFences(p.device,1,&p.fence));
        check(vkResetCommandPool(p.device,p.commands,0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(p.command,&begin));
        vkCmdBindPipeline(p.command,VK_PIPELINE_BIND_POINT_COMPUTE,p.pipeline);
        // The descriptor set remains allocated and bound to the same six buffers.
        vkCmdBindDescriptorSets(p.command,VK_PIPELINE_BIND_POINT_COMPUTE,p.layout,0,1,&p.set,0,nullptr);
        vkCmdPushConstants(p.command,p.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,p.resident_tiles?20:12,push.data());
        if(p.timing_pool){vkCmdResetQueryPool(p.command,p.timing_pool,0,2);vkCmdWriteTimestamp(p.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,p.timing_pool,0);}
        vkCmdDispatch(p.command,static_cast<uint32_t>(seeds.size()),1,1);
        if(p.timing_pool)vkCmdWriteTimestamp(p.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,p.timing_pool,1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(p.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        check(vkEndCommandBuffer(p.command));
        if(is_cancelled()){++p.counters.cancelled_before_dispatch;return false;}
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&p.command;
        check(vkQueueSubmit(p.queue,1,&submit,p.fence));p.submitted=true;++p.counters.submitted;
        if(after_enqueue)after_enqueue();
        check(vkWaitForFences(p.device,1,&p.fence,VK_TRUE,10000000000ULL));p.submitted=false;++p.counters.completed;
        if(p.timing_pool){uint64_t stamps[2]{};check(vkGetQueryPoolResults(p.device,p.timing_pool,0,2,sizeof(stamps),stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            std::cout<<"BENCH_DEVICE_MS="<<double(stamps[1]-stamps[0])*p.timestamp_period/1e6<<"\n";}
        for(size_t i=0;i<check_count;++i){const auto* bytes=static_cast<const uint8_t*>(p.buffers[i+3].mapped);
            if(!std::all_of(bytes+guard_start[i],bytes+capacities[i],[](auto v){return v==0xcd;}))
                throw std::runtime_error("Vulkan output guard or inactive capacity overwritten");}
        if(is_cancelled()){++p.counters.discarded_after_completion;return false;}
        if(copy_offsets || diagnostics) {
            out.offsets.resize(seeds.size()*256);
            std::memcpy(out.offsets.data(),p.buffers[3].mapped,lengths[0]);
        }
        out.final_states.resize(lengths[1]);out.diagnostic_states.resize(lengths[2]);
        std::memcpy(out.final_states.data(),p.buffers[4].mapped,lengths[1]);
        if(diagnostics)std::memcpy(out.diagnostic_states.data(),p.buffers[5].mapped,lengths[2]);
        return true;
    }catch(...){p.failed=true;
        if(p.submitted && vkWaitForFences(p.device,1,&p.fence,VK_TRUE,10000000000ULL)==VK_SUCCESS)p.submitted=false;
        out.offsets.clear();out.final_states.clear();out.diagnostic_states.clear();throw;
    }
}

}
