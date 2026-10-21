// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include "bbport_settings.h"
#include <chrono>
#include <thread>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <crtdbg.h>
int main(int argc, char** argv) {
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    Vulkan::Instance instance{-1,false}; Vulkan::Scheduler scheduler{instance,false};
    if (!instance.IsFsr4Int8Supported()) { puts("SKIP: FSR v07 device features unavailable"); return 0; }
    const bool assets = argc==3;
    // Preparation never records commands or uses frame image handles.
    _putenv_s("BB_FSR4_DIR",assets ? argv[2] : "out/test-fixtures/does-not-contain-fsr-models");
    BbSettings::Get().upscaler = BbSettings::UpscalerFsr4;
    Vulkan::Fsr4Upscaler upscaler{instance,scheduler};
    Vulkan::Fsr4Upscaler::Frame frame{}; frame.output.width=1920; frame.output.height=1080; frame.preset=1;
    bool ready=upscaler.Prepare(frame);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while (upscaler.Preparing() && std::chrono::steady_clock::now()<deadline) {
        assert(!upscaler.Fatal()); assert(upscaler.Problem());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!upscaler.Preparing());
    ready=upscaler.Prepare(frame);
    if (assets) {
        if (!ready) std::fprintf(stderr,"FSR preparation error: %s\n",upscaler.Problem());
        assert(ready && !upscaler.Fatal() && !upscaler.Problem());
        assert(upscaler.Prepare(frame));
        puts("PASS: real FSR model prepared on Vulkan, worker result adopted and reused");
    } else {
        assert(!ready && upscaler.Fatal() && upscaler.Problem());
        puts("PASS: real Vulkan FSR preparation, safe failure and published error");
    }
}
