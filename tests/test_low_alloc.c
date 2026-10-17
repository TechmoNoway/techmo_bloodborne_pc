/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "win32_low_alloc.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL low allocation line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static void *records[8][32];
static DWORD WINAPI worker(void *argument) {
    const size_t slot=(size_t)argument;
    for (size_t i=0;i<32;++i) {
        unsigned char *p=win_low_alloc(513);
        CHECK(p && (uintptr_t)p>=0x10000 && (uintptr_t)p+513<=UINT64_C(0x0800000000));
        CHECK((uintptr_t)p == ((uintptr_t)p & ((UINT64_C(1)<<40)-1)));
        for (size_t j=0;j<513;++j) CHECK(p[j]==0);
        p[512]=(unsigned char)(slot*32+i);
        records[slot][i]=p;
    }
    return 0;
}
int main(void) {
    CHECK(!win_low_alloc(0) && GetLastError()==ERROR_INVALID_PARAMETER);
    CHECK(!win_low_alloc(SIZE_MAX) && GetLastError()==ERROR_INVALID_PARAMETER);
    HANDLE threads[8];
    for (size_t i=0;i<8;++i) { threads[i]=CreateThread(NULL,0,worker,(void *)i,0,NULL); CHECK(threads[i]); }
    CHECK(WaitForMultipleObjects(8,threads,TRUE,INFINITE)==WAIT_OBJECT_0);
    for (size_t i=0;i<256;++i) {
        unsigned char *p=records[i/32][i%32];
        CHECK(p[512]==(unsigned char)i);
        for (size_t j=0;j<i;++j) CHECK(p!=records[j/32][j%32]);
        CHECK(VirtualFree(p,0,MEM_RELEASE));
    }
    for (size_t i=0;i<8;++i) CloseHandle(threads[i]);
    puts("PASS: 256 concurrent zero-filled low records, unique 40-bit lock identities, invalid requests");
}
