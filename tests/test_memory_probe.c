/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "win32_memory.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL memory probe line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
int main(void) {
    DWORD before,after;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for (unsigned i=0;i<128;++i) CHECK(win_mem_probe_section(1024*1024)==0);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)); CHECK(after==before);
    CHECK(win_mem_probe_section(0)==-1 && GetLastError()==ERROR_INVALID_PARAMETER);
    CHECK(win_mem_probe_section(UINT64_MAX)==-1 && GetLastError()==ERROR_INVALID_PARAMETER);
    puts("PASS: actual Windows section/view probes, bounded sizes and no leaked handles");
}
