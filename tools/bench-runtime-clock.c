/* Clock-call throughput only, not a game FPS benchmark. */
#include "host_sync.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const unsigned count = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1000000;
    if (!count) return 1;
    for (unsigned round = 0; round < 5; ++round) {
        uint64_t previous = host_monotonic_ns(), start = previous, sample = previous;
        for (unsigned i = 0; i < count; ++i) {
            sample = host_monotonic_ns();
            if (sample < previous) return 2;
            previous = sample;
        }
        uint64_t end = host_monotonic_ns();
        printf("%u,%.3f\n", round, (double)(end - start) / count);
    }
    return 0;
}
