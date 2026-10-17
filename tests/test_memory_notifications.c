/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercises the real runtime queue and Windows section remapping, without a game.
 * Adapted from DarkIzuku dceac3f478; surviving-view data/protection checks are local. */
#include "../engine/runtime/runtime_memory.c"
#include <assert.h>
#include <crtdbg.h>

/* No guest ABI dispatch or toggle watcher participates in these tests. If one
 * accidentally becomes necessary, fail instead of returning a fake success. */
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid; abort();
}
int32_t *runtime_errno(void) { abort(); }
void runtime_guest_call_sites(uint64_t out[3]) { (void)out; abort(); }
int host_thread_start(HostThread *thread, size_t stack, void *(*entry)(void*), void *argument) {
    (void)thread; (void)stack; (void)entry; (void)argument; abort();
}
void host_sleep_ns(uint64_t ns) { (void)ns; abort(); }

static size_t seen, invalidations, unmaps;
static uintptr_t expected;
static uintptr_t surviving[2];
static void ordered(uintptr_t address, uint64_t size) {
    assert(address==expected++ && size==4);
    ++seen;
}
static void ignore(uintptr_t address, uint64_t size) { (void)address; (void)size; }
static void unmapped(uintptr_t address, uint64_t size) {
    assert(!exclusive_depth && address==surviving[0]+PAGE && size==PAGE);
    ++unmaps;
}
static void invalidate(uintptr_t address, uint64_t size) {
    assert(!exclusive_depth && size==PAGE);
    assert(address==surviving[0] || address==surviving[1]);
    // A remap drops the GPU's read-only tracking. Reapply it only after the
    // runtime reports the surviving range, as the real renderer does.
    DWORD previous;
    assert(VirtualProtect((void*)address,size,PAGE_READONLY,&previous));
    assert(previous==PAGE_READWRITE);
    ++invalidations;
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    hook_map=ordered;
    write_lock();
    for (uintptr_t i=0;i<4097;++i) queue_hook(HOOK_MAP,i,4);
    write_unlock();
    flush_hooks();
    assert(seen==4097 && pending_count==0);
    // Recycle the allocated queue without losing order or retaining callbacks.
    write_lock();
    for (uintptr_t i=4097;i<8194;++i) queue_hook(HOOK_MAP,i,4);
    write_unlock();
    flush_hooks();
    assert(seen==8194 && pending_count==0);

    hook_map=ignore; hook_unmap=unmapped; hook_invalidate=invalidate;
    // This fixture needs only three views, not a reservation of the entire
    // guest VA range (which can contain DLL mappings in a test runner).
    assert(win_mem_space(USER_MIN,USER_MIN+64*UINT64_C(1024)*1024)==0);
    assert(_putenv_s("BB_DMEM_MB","512")==0);
    const uint64_t length=3*PAGE;
    int64_t physical=-1;
    void *mapped=NULL;
    assert(direct_allocate(0,POOL_SIZE,length,PAGE,0,&physical)==0);
    assert(direct_map(&mapped,length,3,0,physical,PAGE)==0);
    surviving[0]=(uintptr_t)mapped;
    surviving[1]=(uintptr_t)mapped+2*PAGE;
    *(uint32_t*)surviving[0]=0x12345678;
    *(uint32_t*)surviving[1]=0x87654321;
    const uint64_t original_generation=__atomic_load_n(runtime_memory_generation(),__ATOMIC_ACQUIRE);
    const uint32_t first_value=0x12345678;
    assert(runtime_memory_write_backing_if_generation(surviving[0],&first_value,4,original_generation));
    DWORD previous;
    assert(VirtualProtect(mapped,length,PAGE_READONLY,&previous));
    invalidations=0;
    // Removing the middle of a Windows section remaps the surviving pieces.
    // Their INVALIDATE notifications used to be discarded together with UNMAP.
    assert(direct_unmap((char*)mapped+PAGE,PAGE)==0);
#ifdef _WIN32
    assert(invalidations==2 && unmaps==1);
    for (unsigned i=0;i<2;++i) {
        MEMORY_BASIC_INFORMATION info;
        assert(VirtualQuery((void*)surviving[i],&info,sizeof(info))==sizeof(info));
        assert(info.State==MEM_COMMIT && info.Protect==PAGE_READONLY);
        assert(runtime_memory_is_mapped(surviving[i],PAGE));
    }
    assert(!runtime_memory_is_mapped(surviving[0]+PAGE,PAGE));
    assert(*(uint32_t*)surviving[0]==0x12345678 && *(uint32_t*)surviving[1]==0x87654321);
    // Invalid requests neither notify nor change the surviving data/protection.
    assert(direct_unmap((char*)mapped+1,PAGE)==INVALID);
    assert(invalidations==2 && unmaps==1 && pending_count==0);
#endif
    assert(pending_count==0);
    hook_unmap=ignore; hook_invalidate=ignore;
    // A GPU readback captured before unmap must not overwrite a surviving or recycled view.
    const uint32_t replacement=0xaabbccdd;
    assert(!runtime_memory_write_backing_if_generation(surviving[0],&replacement,4,original_generation));
    assert(*(uint32_t*)surviving[0]==first_value);
    unsigned char *payload=malloc(length); assert(payload); memset(payload,0x5a,length);
    uint64_t generation=__atomic_load_n(runtime_memory_generation(),__ATOMIC_ACQUIRE);
    assert(!runtime_memory_write_backing_if_generation(surviving[0],payload,length,generation));
    assert(*(uint32_t*)surviving[0]==first_value); // Full range validation before any store.
    void *recycled=(void*)(surviving[0]+PAGE);
    assert(direct_map(&recycled,PAGE,3,0x10,physical+PAGE,PAGE)==0);
    assert((uintptr_t)recycled==surviving[0]+PAGE);
    *(uint32_t*)recycled=0x10203040;
    assert(!runtime_memory_write_backing_if_generation((uintptr_t)recycled,&replacement,4,generation));
    assert(*(uint32_t*)recycled==0x10203040);
    generation=__atomic_load_n(runtime_memory_generation(),__ATOMIC_ACQUIRE);
    assert(runtime_memory_write_backing_if_generation(surviving[0],payload,length,generation));
    assert(*(uint32_t*)surviving[0]==0x5a5a5a5a && *(uint32_t*)surviving[1]==0x5a5a5a5a);
    assert(!runtime_memory_write_backing_if_generation(UINTPTR_MAX-2,payload,8,generation));
    assert(!runtime_memory_write_backing_if_generation(surviving[0],payload,4,generation|1));
    free(payload);
    assert(direct_release(physical,length)==0);
    free(pending); pending=NULL; pending_capacity=0;
    puts("PASS: 8194 ordered notifications, queue reuse, exactly one early unmap, "
         "two surviving-view invalidations, restored tracking, generation-guarded unmap/remap and all-or-nothing backing stores");
}
