/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "win32_low_alloc.h"
#include "../address_space.h"

void *win_low_alloc(size_t size) {
    /* Guest placeholders start at 32 GiB. Thread identities stay below them:
     * the game stores lock owners in only 40 bits. */
    const uintptr_t limit=BB_HOST_LOW_LIMIT;
    if (!size || size>limit-UINT64_C(0x10000)) {
        SetLastError(ERROR_INVALID_PARAMETER); return NULL;
    }
    typedef PVOID (WINAPI *Alloc2)(HANDLE,PVOID,SIZE_T,ULONG,ULONG,MEM_EXTENDED_PARAMETER *,ULONG);
    HMODULE kernel=GetModuleHandleW(L"kernelbase.dll");
    Alloc2 allocate=kernel ? (Alloc2)(void *)GetProcAddress(kernel,"VirtualAlloc2") : NULL;
    if (!allocate) { SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
    MEM_ADDRESS_REQUIREMENTS range={0};
    range.LowestStartingAddress=(void *)(uintptr_t)0x10000;
    range.HighestEndingAddress=(void *)(limit-1);
    MEM_EXTENDED_PARAMETER parameter={0};
    parameter.Type=MemExtendedParameterAddressRequirements;
    parameter.Pointer=&range;
    void *memory=allocate(GetCurrentProcess(),NULL,size,MEM_RESERVE|MEM_COMMIT,
                          PAGE_READWRITE,&parameter,1);
    if (memory && ((uintptr_t)memory>=limit || size>limit-(uintptr_t)memory)) {
        VirtualFree(memory,0,MEM_RELEASE);
        SetLastError(ERROR_INVALID_ADDRESS); return NULL;
    }
    return memory;
}
#endif
