/* Synthetic native exception fixture. Never loads game code or a renderer. */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "engine/runtime/win32_diagnostics.h"

static int guest_marker;
static LONG WINAPI fault(EXCEPTION_POINTERS *exception) {
    bb_diagnostics_fault(exception, guest_marker);
    ExitProcess(139);
}

static DWORD WINAPI write_fault(void *unused) {
    (void)unused;
    volatile unsigned *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
    if (!page) ExitProcess(2);
    *page = 42;
    return 1;
}

static DWORD WINAPI lock_stderr(void *ready) {
    _lock_file(stderr);
    SetEvent((HANDLE)ready);
    Sleep(INFINITE);
    return 1;
}

int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    /* Test-only stand-in executable: the real reporter has no such controls. */
    if (argc == 5) {
        const char *mode = getenv("BB_DIAGNOSTIC_FIXTURE_HELPER");
        if (!mode || !strcmp(mode, "exit")) return 71;
        if (!strcmp(mode, "timeout")) { Sleep(10000); return 71; }
        HANDLE mapping = (HANDLE)(uintptr_t)strtoull(argv[1], NULL, 10);
        HANDLE ready = (HANDLE)(uintptr_t)strtoull(argv[2], NULL, 10);
        HANDLE requested = (HANDLE)(uintptr_t)strtoull(argv[3], NULL, 10);
        BbCrashRequest *request = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*request));
        if (!request) return 72;
        request->startup_error = !strcmp(mode, "startup-error") ? ERROR_ACCESS_DENIED : 0;
        SetEvent(ready);
        if (!request->startup_error) WaitForSingleObject(requested, 10000);
        return 73;
    }
    bb_diagnostics_init();
    SetUnhandledExceptionFilter(fault);
    if (argc == 2 && !strcmp(argv[1], "--normal")) return 0;
    if (argc == 2 && !strcmp(argv[1], "--fatal")) {
        bb_diagnostics_fatal("Synthetic \"assertion\"\\path\nsecond line");
        ExitProcess(23);
    }
    if (argc == 2 && !strcmp(argv[1], "--worker")) {
        HANDLE thread = CreateThread(NULL, 0, write_fault, NULL, 0, NULL);
        if (!thread) return 2;
        WaitForSingleObject(thread, INFINITE);
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "--write")) return (int)write_fault(NULL);
    if (argc == 2 && !strcmp(argv[1], "--locked-stderr")) {
        HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ready || !CreateThread(NULL, 0, lock_stderr, ready, 0, NULL)) return 2;
        WaitForSingleObject(ready, INFINITE);
        return (int)write_fault(NULL);
    }
    if (argc == 2 && !strcmp(argv[1], "--guest")) {
        /* Validate the guest-origin reporting contract without loading game code. */
        guest_marker = 1;
        ULONG_PTR parameters[] = {1, 8};
        RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, parameters);
    }
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, NULL);
    return 1;
}
