#pragma once
#ifdef _WIN32
#include <windows.h>
#include <stdint.h>

/* Shared only with the prestarted crash reporter; no game/account data copied here. */
typedef struct {
    DWORD protocol, structure_size;
    volatile LONG startup_error, dump_error, status_error, dump_success;
    DWORD process_id, thread_id, exception_code, access_kind;
    uint64_t exception_pointers, instruction_address, access_address;
    wchar_t directory[2048];
} BbCrashRequest;
#define BB_CRASH_PROTOCOL 2u

#ifdef __cplusplus
extern "C" {
#endif
void bb_diagnostics_init(void);
void bb_diagnostics_fault(EXCEPTION_POINTERS *exception, int guest);
/* Synthetic exception for fatal host assertions; returns after the bounded dump wait. */
void bb_diagnostics_fatal(const char *reason);
#define BB_RENDERER_ASSERT_EXCEPTION ((DWORD)0xe0424223)
#ifdef __cplusplus
}
#endif
#endif
