#pragma once
#ifdef _WIN32
#include <windows.h>
#include <stdint.h>

/* Shared only with the prestarted crash reporter; no game/account data copied here. */
typedef struct {
    DWORD process_id, thread_id, exception_code, access_kind;
    uint64_t exception_pointers, instruction_address, access_address;
    wchar_t directory[2048];
} BbCrashRequest;

void bb_diagnostics_init(void);
void bb_diagnostics_fault(EXCEPTION_POINTERS *exception, int guest);
#endif
