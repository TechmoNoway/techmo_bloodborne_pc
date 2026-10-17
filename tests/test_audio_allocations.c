/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise the production AJM job, injecting only concatenation allocation failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
static int fail_allocation, allocations, released, verify_bytes;
static unsigned char expected[8];
static size_t expected_size;
static void *joined_pointer;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL audio line %d: %s\n",__LINE__,#x); ExitProcess(1); } } while (0)
static void *checked_malloc(size_t size) {
    ++allocations;
    if (fail_allocation) return NULL;
    return joined_pointer=malloc(size);
}
static void checked_free(void *p) {
    if (p && p==joined_pointer) {
        if (verify_bytes) CHECK(!memcmp(p,expected,expected_size));
        ++released; joined_pointer=NULL;
    }
    free(p);
}
#define malloc checked_malloc
#define free checked_free
#include "../engine/runtime/runtime_ajm.c"
#undef malloc
#undef free
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *nid) {
    (void)table; (void)count; (void)nid; CHECK(0); return 0;
}

int main(void) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    Context *ctx=calloc(1,sizeof(*ctx)); CHECK(ctx);
    ctx->instances[1].used=1;
    unsigned char a[]={1,2,3}, b[]={4,5,6,7};
    SidebandResult result;
    Job job={0}; job.input_count=2;
    job.inputs[0]=(ChunkBuffer){.address=a,.size=sizeof(a)};
    job.inputs[1]=(ChunkBuffer){.address=b,.size=sizeof(b)};
    job.output_control=(ChunkBuffer){.address=&result,.size=sizeof(result)};
    memcpy(expected,a,sizeof(a)); memcpy(expected+sizeof(a),b,sizeof(b));
    expected_size=7; verify_bytes=1;
    run_job(ctx,1,&job);
    CHECK(allocations==1 && released==1 && !joined_pointer);
    CHECK(result.result==RESULT_NOT_INITIALIZED);
    fail_allocation=1; run_job(ctx,1,&job);
    CHECK(allocations==2 && result.result==RESULT_INVALID_PARAMETER && !joined_pointer);
    fail_allocation=0; verify_bytes=0;
    job.inputs[0].size=UINT32_MAX; job.inputs[1].size=1;
    run_job(ctx,1,&job);
    CHECK(allocations==2 && result.result==RESULT_INVALID_PARAMETER);
    job.inputs[0].size=64u*1024*1024; job.inputs[1].size=1;
    run_job(ctx,1,&job);
    CHECK(allocations==2 && result.result==RESULT_INVALID_PARAMETER);
    job.inputs[0]=(ChunkBuffer){.address=NULL,.size=1};
    run_job(ctx,1,&job);
    CHECK(allocations==2 && result.result==RESULT_INVALID_PARAMETER);
    job.input_count=1; run_job(ctx,1,&job);
    CHECK(result.result==RESULT_INVALID_PARAMETER);
    job.inputs[0]=(ChunkBuffer){.address=a,.size=sizeof(a)};
    run_job(ctx,1,&job);
    CHECK(allocations==2 && result.result==RESULT_NOT_INITIALIZED);
    job.input_count=2; job.inputs[0]=(ChunkBuffer){0}; job.inputs[1]=(ChunkBuffer){0};
    run_job(ctx,1,&job);
    CHECK(allocations==3 && released==2 && result.result==0);
    free(ctx);
    puts("PASS: AJM concatenation ownership, failed allocation, overflow, 64 MiB bound, null/empty input and single-buffer path");
    return 0;
}
