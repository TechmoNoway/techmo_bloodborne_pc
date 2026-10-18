// SPDX-License-Identifier: GPL-3.0-or-later
#include "performance_monitor.h"
#include <chrono>
#include <cstdlib>
#include <limits>
#include <thread>

static void Check(bool value,const char* text) {
    if(!value){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}
}
int main(int argc,char** argv) {
    using namespace PerformanceMonitor;
    const Adapter id{.low=0x12345678,.high=0xabcdef01,.node_mask=1,.valid=true};
    const auto parsed=ParseEngine(L"pid_123_luid_0xABCDEF01_0x12345678_phys_0_eng_2_engtype_Compute_0");
    Check(parsed && parsed->high==id.high && parsed->low==id.low && parsed->physical==0 && parsed->index==2,"Windows LUID field order and engine parsing");
    for(const auto* bad:{L"_Total",L"pid_1_luid_0x_0x1_phys_0_eng_0_engtype_3D",
        L"pid_1_luid_0x100000000_0x1_phys_0_eng_0_engtype_3D",
        L"pid_1_luid_0x0_0x1_phys_0_eng_4294967296_engtype_3D",
        L"pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_"})
        Check(!ParseEngine(bad),"Malformed or overflowing counter instance is rejected");
    GpuTotal total(id);
    total.Add(L"pid_1_luid_0xabcdef01_0x12345678_phys_0_eng_2_engtype_Compute",40);
    total.Add(L"pid_2_luid_0xabcdef01_0x12345678_phys_0_eng_2_engtype_Compute",35);
    total.Add(L"pid_1_luid_0xabcdef01_0x12345678_phys_0_eng_3_engtype_Copy",60);
    total.Add(L"pid_1_luid_0xabcdef01_0x99999999_phys_0_eng_2_engtype_3D",100);
    total.Add(L"pid_1_luid_0xabcdef01_0x12345678_phys_1_eng_2_engtype_3D",100);
    Check(total.Value()==75,"GPU uses the busiest engine of the selected adapter and node, with processes combined");
    total.Add(L"pid_3_luid_0xabcdef01_0x12345678_phys_0_eng_2_engtype_Compute",45);
    Check(total.Value()==100,"GPU load never exceeds 100 percent");
    GpuTotal unavailable(id);
    unavailable.Add(L"pid_1_luid_0xabcdef01_0x12345678_phys_0_eng_2_engtype_Compute",std::numeric_limits<double>::quiet_NaN());
    unavailable.Add(L"pid_1_luid_0xabcdef01_0x12345678_phys_0_eng_2_engtype_Compute",-1);
    Check(unavailable.Value()==-1,"Missing or invalid GPU data remains unavailable instead of becoming zero");
    Check(OverlayText(true,true,true,60,16.7,"dlss",{42,95})=="60 FPS  16.7 ms  dlss  |  CPU 42%  |  GPU 95%","Overlay combines the selected counters");
    Check(OverlayText(false,true,true,0,0,"off",{})=="CPU N/A  |  GPU N/A","CPU and GPU can be displayed without FPS and show unavailable data honestly");
    Check(OverlayText(false,false,false,0,0,"off",{}).empty(),"All-off hides the performance overlay");
    Check(OverlayText(false,false,true,0,0,"off",{0,0})=="GPU 0%","Valid zero load is distinct from missing counters");
    {
        Monitor monitor(id);
        monitor.Request(false,false);
        Check(monitor.Read().cpu<0 && monitor.Read().gpu<0,"Disabled monitor has no stale samples");
    }
    if(argc>1 && std::string_view(argv[1])=="--live") {
        Monitor monitor({}); // CPU-only Windows sampling does not require a GPU or game.
        monitor.Request(true,false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));
        const auto data=monitor.Read();
        Check(data.cpu>=0 && data.cpu<=100 && data.gpu<0,"Live system CPU counter produces bounded data");
        std::printf("Windows CPU sample: %.1f%%\n",data.cpu);
        monitor.Request(false,false);
        Check(monitor.Read().cpu<0 && monitor.Read().gpu<0,"Turning metrics off immediately hides cached values");
        // Rapid toggles exercise both the idle wake-up and shutdown paths.
        for(int i=0;i<50;++i){monitor.Request(true,false);monitor.Request(false,false);}
    }
    std::puts("PASS: adapter filtering, GPU aggregation, N/A, overlay combinations and monitor lifecycle");
}
