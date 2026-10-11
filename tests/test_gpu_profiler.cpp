// Actual headless Vulkan queries; no game, swapchain or user settings.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    _putenv_s("BB_GPU_PROFILE", "1");
    _putenv_s("BB_GPU_LOG", argc > 1 ? argv[1] : "");
#else
    setenv("BB_GPU_PROFILE", "1", 1);
    if (argc > 1) setenv("BB_GPU_LOG", argv[1], 1);
    else unsetenv("BB_GPU_LOG");
#endif
    Vulkan::Instance instance{-1, false};
    Vulkan::Scheduler scheduler{instance, false};
    Vulkan::GpuProfiler::Init(instance, scheduler);
    auto* profiler = Vulkan::GpuProfiler::Get();
    assert(profiler);
    // Deliberately don't submit. The old WAIT query path hangs when the ring wraps;
    // the new path must return, preserve the queries and skip diagnostic samples.
    for (unsigned i = 0; i < 12; ++i) {
        profiler->Mark(0x1234, [] { return "headless query regression"; });
        profiler->BeginFrame();
    }
    assert(profiler->CollectedFrames() == 0 && profiler->SkippedFrames() == 9);
    scheduler.Finish();
    // Revisit every preserved slice only after the submission completes.
    for (unsigned i = 0; i < 4; ++i) profiler->BeginFrame();
    assert(profiler->CollectedFrames() == 4 && profiler->SkippedFrames() == 9);
    // Repeated reuse must not read stale results or reset an in-flight pool.
    for (unsigned i = 0; i < 20; ++i) {
        profiler->Mark(0x5678, [] { return "reused query slice"; });
        profiler->BeginFrame();
        scheduler.Finish();
    }
    assert(profiler->CollectedFrames() >= 20);
    Vulkan::GpuProfiler::Shutdown();
    assert(!Vulkan::GpuProfiler::Get());
    puts("PASS: nonblocking GPU profiler, pending-slice preservation and safe query reuse");
}
