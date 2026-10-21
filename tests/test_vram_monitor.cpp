// SPDX-License-Identifier: GPL-3.0-or-later
// --live adds a disposable Vulkan allocation on the selected discrete GPU.
#if __has_include("../engine/overlay/vram_monitor.h")
#include "../engine/overlay/vram_monitor.h"
#else
#include "../engine/renderer/shim/vram_monitor.h"
#endif
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include <vulkan/vulkan.h>

static void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
static void Live() {
    VkApplicationInfo app{.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo create{.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance{};
    Check(vkCreateInstance(&create,nullptr,&instance)==VK_SUCCESS,"Vulkan instance");
    std::uint32_t count{};
    Check(vkEnumeratePhysicalDevices(instance,&count,nullptr)==VK_SUCCESS && count>0,"physical GPUs available");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance,&count,devices.data())==VK_SUCCESS,"enumerate GPUs");
    VkPhysicalDevice gpu=devices[0];
    for (auto candidate:devices) { VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(candidate,&p); if (p.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { gpu=candidate; break; } }
    VkPhysicalDeviceIDProperties id{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&id};
    vkGetPhysicalDeviceProperties2(gpu,&props);
    if (props.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        VideoMemory::Monitor uma({});uma.Request(true);
        Check(!VideoMemory::Known(uma.Read()),"UMA local memory is not dedicated VRAM");
        std::puts("Integrated GPU: dedicated VRAM N/A; shared RAM excluded");
        vkDestroyInstance(instance,nullptr);return;
    }
    VideoMemory::Adapter selected{.nodes=id.deviceNodeMask,.valid=bool(id.deviceLUIDValid)};
    std::memcpy(&selected.low,id.deviceLUID,4);std::memcpy(&selected.high,id.deviceLUID+4,4);
    Check(selected.valid,"Vulkan adapter LUID");
    std::uint32_t queues{};vkGetPhysicalDeviceQueueFamilyProperties(gpu,&queues,nullptr);
    std::vector<VkQueueFamilyProperties> families(queues);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&queues,families.data());
    std::uint32_t family=0;while(family<queues && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))++family;
    Check(family<queues,"graphics queue");
    float priority=1;
    VkDeviceQueueCreateInfo qc{.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dc{.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qc};
    VkDevice device{};Check(vkCreateDevice(gpu,&dc,nullptr,&device)==VK_SUCCESS,"logical device");
    VkBufferCreateInfo bc{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=64*1024*1024,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer{};Check(vkCreateBuffer(device,&bc,nullptr,&buffer)==VK_SUCCESS,"64 MiB buffer");
    VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,buffer,&requirements);
    VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(gpu,&memory);
    std::uint32_t type=0;
    while(type<memory.memoryTypeCount && (!(requirements.memoryTypeBits & (1u<<type)) || !(memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))++type;
    Check(type<memory.memoryTypeCount,"device-local memory type");
    VkMemoryAllocateInfo allocation{.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=requirements.size,.memoryTypeIndex=type};
    VkDeviceMemory storage{};Check(vkAllocateMemory(device,&allocation,nullptr,&storage)==VK_SUCCESS,"allocate GPU memory");
    Check(vkBindBufferMemory(device,buffer,storage,0)==VK_SUCCESS,"bind buffer memory");
    VkCommandPoolCreateInfo pc{.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool pool{};Check(vkCreateCommandPool(device,&pc,nullptr,&pool)==VK_SUCCESS,"command pool");
    VkCommandBufferAllocateInfo ca{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command{};Check(vkAllocateCommandBuffers(device,&ca,&command)==VK_SUCCESS,"command buffer");
    VkCommandBufferBeginInfo begin{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS,"begin transfer");vkCmdFillBuffer(command,buffer,0,VK_WHOLE_SIZE,0x1234);
    Check(vkEndCommandBuffer(command)==VK_SUCCESS,"end transfer");
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkSubmitInfo submit{.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    Check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS && vkQueueWaitIdle(queue)==VK_SUCCESS,"GPU transfer completes");
    {
        VideoMemory::Monitor monitor(selected);monitor.Request(true);
        VideoMemory::Sample result;
        for(int attempt=0;attempt<20;++attempt) { std::this_thread::sleep_for(std::chrono::milliseconds(250));result=monitor.Read();if(VideoMemory::Known(result)&&result.used>=64*1024*1024)break; }
        std::printf("GPU %s LUID %08x:%08x: %s (%.0f bytes used, %.0f bytes physical)\n",props.properties.deviceName,selected.high,selected.low,VideoMemory::Text(result).c_str(),result.used,result.total);
        Check(VideoMemory::Known(result) && result.used>=64*1024*1024,"VRAM reflects this process's real Vulkan allocation");
        monitor.Request(false);Check(!VideoMemory::Known(monitor.Read()),"disable hides live sample immediately");
        monitor.Request(true);
        for(int attempt=0;attempt<20 && !VideoMemory::Known(monitor.Read());++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        Check(VideoMemory::Known(monitor.Read()),"worker resumes after re-enable");
    }
    // Another LUID must never fall back to the first/system adapter.
    auto wrong=selected;wrong.low^=0x80000000;wrong.high^=0x80000000;
    VideoMemory::Query wrong_query;
    Check(!wrong_query.Open(wrong),"unknown LUID never selects another adapter");
    VideoMemory::Monitor unavailable(wrong);unavailable.Request(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Check(!VideoMemory::Known(unavailable.Read()),"invalid render LUID reports unavailable");
    vkDestroyCommandPool(device,pool,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,storage,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
}
int main(int argc,char** argv) {
    using namespace VideoMemory;
    Check(Text({})=="VRAM N/A","unknown is not zero");
    Check(Text({0,8*1073741824.})=="VRAM 0.0 / 8.0 GiB","valid zero and binary units");
    Check(Text({3.5*1073741824.,8*1073741824.})=="VRAM 3.5 / 8.0 GiB","used / physical capacity");
    Check(Text({9*1073741824.,8*1073741824.})=="VRAM 9.0 / 8.0 GiB","do not hide overcommit by clamping");
    Check(!Known({0,0}) && !Known({1,-1}) && !Known({-1,1}),"unavailable or UMA capacity is not RAM");
    Check(!Known({std::numeric_limits<double>::infinity(),1}) && !Known({0,std::numeric_limits<double>::quiet_NaN()}),"invalid samples");
    Adapter selected{0x12345678,0xabcdef01,1,true};
    Check(Matches(selected,0x12345678,0xabcdef01),"full selected adapter LUID");
    Check(!Matches(selected,0x12345678,0) && !Matches(selected,0,0xabcdef01),"both LUID halves must match");
    selected.nodes=0;Check(!Matches(selected,0x12345678,0xabcdef01),"no physical node is not an adapter");
    Monitor monitor({});Check(!Known(monitor.Read()),"all-off before worker creation");
    for(int i=0;i<20;++i) { monitor.Request(true);Check(!Known(monitor.Read()),"unknown adapter remains unknown");monitor.Request(false);Check(!Known(monitor.Read()),"toggle generations hide stale values"); }
    {
        Monitor shared({});std::vector<std::jthread> presenters;
        for(int i=0;i<8;++i) presenters.emplace_back([&]{for(int frame=0;frame<128;++frame) { shared.Request((frame&1)!=0); (void)shared.Read(); }});
        presenters.clear();shared.Request(false);
        Check(!Known(shared.Read()),"concurrent presenters can toggle and stop a single sampler safely");
    }
    if(argc>1 && std::string(argv[1])=="--live")Live();
    std::puts("PASS: VRAM units, missing/zero/overcommit, render adapter selection, toggle lifecycle");
}
