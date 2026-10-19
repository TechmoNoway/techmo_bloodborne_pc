// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

// SDR display light (linear Rec.709) -> absolute HDR10 (Rec.2020 / ST.2084).
// This is an output conversion, not recovered scene-referred HDR information.
namespace HdrOutput {
inline float Pq(float nits) {
    const float p = std::pow(std::clamp(nits / 10000.f, 0.f, 1.f), 2610.f / 16384.f);
    return std::pow((3424.f / 4096.f + 2413.f / 128.f * p) /
                       (1.f + 2392.f / 128.f * p), 2523.f / 32.f);
}
inline std::array<float, 3> Convert(std::array<float, 3> rgb, float white,
                                     float peak, bool expand) {
    for (auto& c : rgb) c = std::clamp(c, 0.f, 1.f);
    const float y = rgb[0] * .2126f + rgb[1] * .7152f + rgb[2] * .0722f;
    const float shoulder = std::clamp((y - .75f) / .25f, 0.f, 1.f);
    const float scale = y > 0.f ? (y * white + (expand ? shoulder * shoulder * (peak - white) : 0.f)) / y : 0.f;
    for (auto& c : rgb) c = std::clamp(c * scale, 0.f, peak);
    return {Pq(.6274039f * rgb[0] + .3292830f * rgb[1] + .0433131f * rgb[2]),
            Pq(.0690973f * rgb[0] + .9195404f * rgb[1] + .0113623f * rgb[2]),
            Pq(.0163914f * rgb[0] + .0880133f * rgb[1] + .8955953f * rgb[2])};
}
inline float SrgbToLinear(float c) {
    return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
}
} // namespace HdrOutput
