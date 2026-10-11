#include "host_sync.h"
#include <assert.h>
#include <stdio.h>

static HostMutex start_mutex = HOST_MUTEX_INIT;
static HostCond start_condition = HOST_COND_INIT;
static unsigned ready, proceed;

static void *clock_reader(void *unused) {
    (void)unused;
    host_lock(&start_mutex);
    ++ready;
    host_cond_broadcast(&start_condition);
    while (!proceed) host_cond_wait(&start_condition, &start_mutex);
    host_unlock(&start_mutex);
    uint64_t previous = host_monotonic_ns();
    for (unsigned i = 0; i < 100000; ++i) {
        uint64_t sample = host_monotonic_ns();
        assert(sample >= previous);
        previous = sample;
    }
    return NULL;
}

int main(void) {
    /* Concurrent first use exercises frequency initialization as well as monotonicity. */
    HostThread workers[8];
    for (unsigned i = 0; i < 8; ++i) assert(host_thread_start(&workers[i], 0, clock_reader, NULL) == 0);
    host_lock(&start_mutex);
    while (ready != 8) host_cond_wait(&start_condition, &start_mutex);
    proceed = 1;
    host_cond_broadcast(&start_condition);
    host_unlock(&start_mutex);
    for (unsigned i = 0; i < 8; ++i) assert(host_thread_join(workers[i]) == 0);
    const uint64_t intervals[] = {0, 1, 100, 999, 100000, 600000, 2000000};
    for (unsigned i = 0; i < sizeof(intervals) / sizeof(intervals[0]); ++i) {
        uint64_t deadline = host_monotonic_ns() + intervals[i];
        host_sleep_until_ns(deadline);
        assert(host_monotonic_ns() >= deadline);
    }
#ifdef _WIN32
    LARGE_INTEGER frequency, before, after;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&before);
    uint64_t converted = host_monotonic_ns();
    QueryPerformanceCounter(&after);
    uint64_t lo = (uint64_t)((__uint128_t)before.QuadPart * 1000000000 / frequency.QuadPart);
    uint64_t hi = (uint64_t)((__uint128_t)after.QuadPart * 1000000000 / frequency.QuadPart);
    assert(converted >= lo && converted <= hi);
#endif
    puts("PASS: concurrent monotonic clocks, QPC conversion and no early sleep return");
    return 0;
}
