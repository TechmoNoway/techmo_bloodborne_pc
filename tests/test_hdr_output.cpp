// SPDX-License-Identifier: GPL-3.0-or-later
#include "hdr_output.h"
#include <cstdio>
#include <cstdlib>

static void Check(bool pass, const char* reason) {
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
int main() {
    using namespace HdrOutput;
    Check(Pq(0) < .000001f, "PQ black");
    Check(std::abs(Pq(100) - .5080784f) < .00001f, "ST.2084 100 nits");
    Check(std::abs(Pq(1000) - .7518271f) < .00001f, "ST.2084 1000 nits");
    Check(std::abs(Pq(10000) - 1.f) < .000001f, "PQ upper bound");
    const auto white = Convert({1, 1, 1}, 203, 1000, false);
    for (auto c : white) Check(std::abs(c - Pq(203)) < .00001f, "Reference white and neutral primaries");
    const auto peak = Convert({1, 1, 1}, 203, 1000, true);
    for (auto c : peak) Check(std::abs(c - Pq(1000)) < .00001f, "Optional highlight expansion");
    const auto mid = Convert({.5f, .5f, .5f}, 203, 1000, true);
    Check(std::abs(mid[0] - Pq(101.5f)) < .00001f, "Expansion preserves midtones");
    const auto red = Convert({1, 0, 0}, 203, 1000, false);
    Check(std::abs(red[0] - Pq(203 * .6274039f)) < .00001f && red[0] > red[1] && red[1] > red[2], "Rec.709 to Rec.2020 red");
    float last = -1;
    for (int i = 0; i <= 1000; ++i) {
        const float x = i / 1000.f;
        const auto c = Convert({x, x, x}, 203, 1000, true);
        Check(std::isfinite(c[0]) && c[0] >= last && c[0] <= 1, "Monotonic bounded finite output");
        last = c[0];
    }
    Check(std::abs(SrgbToLinear(.5f) - .2140411f) < .000001f, "Overlay sRGB decoding");
    std::puts("PASS: HDR10 transfer, primaries, reference white, expansion and overlay color");
}
