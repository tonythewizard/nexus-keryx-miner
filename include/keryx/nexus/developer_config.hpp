#pragma once
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <regex>
// Installation identities belong in local CMake cache values, never in Git.
// Inert defaults preserve refusal when a developer has not configured a GPU.
#ifndef NEXUS_PRIMARY_GPU_UUID
#define NEXUS_PRIMARY_GPU_UUID "GPU-00000000-0000-0000-0000-000000000000"
#endif
#ifndef NEXUS_SECONDARY_GPU_ID
#define NEXUS_SECONDARY_GPU_ID "pci:0000:00:00.0"
#endif

namespace keryx::nexus {
inline const std::string& primary_gpu_uuid() {
    static const std::string id=[] {
        const char* value=std::getenv("NEXUS_PRIMARY_GPU_UUID");
        std::string selected=value && *value ? value : NEXUS_PRIMARY_GPU_UUID;
        if(!std::regex_match(selected,std::regex("GPU-[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}")) ||
            selected=="GPU-00000000-0000-0000-0000-000000000000")
            throw std::runtime_error("Explicit NEXUS_PRIMARY_GPU_UUID required");
        return selected;
    }();
    return id;
}
inline std::string developer_device_lease_name(bool primary) {
    std::string id = primary ? primary_gpu_uuid() : NEXUS_SECONDARY_GPU_ID;
    std::replace(id.begin(), id.end(), ':', '_');
    std::replace(id.begin(), id.end(), '.', '_');
    return "lease-" + id + ".lock";
}
inline std::filesystem::path developer_data_root() {
    const char* value = std::getenv("NEXUS_DATA_DIR");
    if (!value || !*value || !std::filesystem::path(value).is_absolute())
        throw std::runtime_error("An absolute private NEXUS_DATA_DIR is required");
    return value;
}
}
