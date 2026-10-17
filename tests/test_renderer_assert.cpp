// SPDX-License-Identifier: GPL-2.0-or-later
// The production renderer assertion, without a Vulkan device or game.
#include "common/assert.h"
#include "../engine/runtime/win32_diagnostics.h"
#include <barrier>
#include <cstring>
#include <thread>
static void fail() { ASSERT_MSG(false,"Intentional renderer assertion fixture"); }
int main(int argc,char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    bb_diagnostics_init();
    if (argc>1 && !std::strcmp(argv[1],"worker")) {
        std::jthread worker(fail);
    } else if (argc>1 && !std::strcmp(argv[1],"concurrent")) {
        std::barrier ready{3};
        auto run=[&] { ready.arrive_and_wait(); fail(); };
        std::jthread first(run),second(run); ready.arrive_and_wait();
    } else fail();
    return 99;
}
