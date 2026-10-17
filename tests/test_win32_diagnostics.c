/* Synthetic native exception fixture. Never loads game code or a renderer. */
#include <windows.h>
#include <string.h>
#include "engine/runtime/win32_diagnostics.h"

static LONG WINAPI fault(EXCEPTION_POINTERS *exception) {
    bb_diagnostics_fault(exception, 0);
    ExitProcess(139);
}

int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    bb_diagnostics_init();
    SetUnhandledExceptionFilter(fault);
    if (argc == 2 && !strcmp(argv[1], "--normal")) return 0;
    if (argc == 2 && !strcmp(argv[1], "--fatal")) {
        bb_diagnostics_fatal("Synthetic \"assertion\"\\path\nsecond line");
        ExitProcess(23);
    }
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, NULL);
    return 1;
}
