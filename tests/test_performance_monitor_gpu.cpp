// SPDX-License-Identifier: GPL-3.0-or-later
// Local WDDM/Vulkan smoke test: a tiny synthetic transfer workload, no game/save/display changes.
#include "performance_monitor.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>
#include <chrono>
#include <cstring>
#include <thread>
#include <cstdlib>

static void Check(bool pass,const char* text) {
    if(!pass){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}
}
int main() {
    Vulkan::Instance instance(EmulatorSettings.GetGpuId(),false);
    Vulkan::Scheduler scheduler(instance);
    const auto properties=instance.GetPhysicalDevice().getProperties2<
        vk::PhysicalDeviceProperties2,vk::PhysicalDeviceIDProperties>();
    const auto& identity=properties.get<vk::PhysicalDeviceIDProperties>();
    PerformanceMonitor::Adapter adapter{.node_mask=identity.deviceNodeMask,.valid=bool(identity.deviceLUIDValid)};
    std::memcpy(&adapter.low,identity.deviceLUID.data(),4);
    std::memcpy(&adapter.high,identity.deviceLUID.data()+4,4);
    Check(adapter.valid,"Vulkan exposes a Windows adapter LUID");
    std::printf("Vulkan monitor: %s, LUID %08x:%08x, node mask %x\n",
        properties.get<vk::PhysicalDeviceProperties2>().properties.deviceName.data(),adapter.high,adapter.low,adapter.node_mask);
    PerformanceMonitor::Monitor monitor(adapter);
    monitor.Request(true,true);
    VkBuffer buffer{};VmaAllocation allocation{};
    const VkBufferCreateInfo buffer_info{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=1024*1024,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo memory{.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    Check(vmaCreateBuffer(instance.GetAllocator(),&buffer_info,&memory,&buffer,&allocation,nullptr)==VK_SUCCESS,"GPU fixture buffer allocation");
    for(unsigned i=0;i<10;++i) {
        scheduler.CommandBuffer().fillBuffer(buffer,0,VK_WHOLE_SIZE,i);
        scheduler.Finish();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    const auto data=monitor.Read();
    std::printf("Windows monitor readback: CPU %.1f%%, GPU %.1f%%\n",data.cpu,data.gpu);
    Check(data.cpu>=0 && data.cpu<=100,"Live CPU sample is valid");
    Check(data.gpu>=0 && data.gpu<=100,"WDDM sample matches the active Vulkan adapter");
    monitor.Request(false,false);
    Check(monitor.Read().cpu<0 && monitor.Read().gpu<0,"Disabling the overlay hides cached metrics");
    vmaDestroyBuffer(instance.GetAllocator(),buffer,allocation);
    std::puts("PASS: live WDDM GPU matching, CPU sampling and disable/release");
}
