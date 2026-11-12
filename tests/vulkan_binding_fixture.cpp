// CPU-only shared-library fixture: never opens a Vulkan loader or GPU.
#include <cstring>
#include <cstddef>
extern "C" int keryx_llama_vk_select_device(const char* identity,char* uuid,std::size_t cap){
  constexpr char selected[]="GPU-00000000-0000-0000-0000-000000000000";
  if(!identity || std::strcmp(identity,"pci:0000:00:00.0") || cap<sizeof(selected))return -1;
  std::memcpy(uuid,selected,sizeof(selected));return 0;
}
