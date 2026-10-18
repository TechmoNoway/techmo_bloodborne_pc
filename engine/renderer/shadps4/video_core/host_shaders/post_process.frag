// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    bool sdr_to_hdr;
    float hdr_white;
    float hdr_peak;
    float hdr_expand;
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr) {
        color = color_linear;
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        if (pp.sdr_to_hdr) {
            vec3 rgb = clamp(color_linear.rgb, 0.0, 1.0);
            float y = dot(rgb, vec3(.2126, .7152, .0722));
            float shoulder = clamp((y - .75) / .25, 0.0, 1.0);
            float scale = y > 0.0 ? (y * pp.hdr_white + pp.hdr_expand * shoulder * shoulder * (pp.hdr_peak - pp.hdr_white)) / y : 0.0;
            rgb = clamp(rgb * scale, 0.0, pp.hdr_peak);
            // GLSL matrices are specified column-first: linear Rec.709 -> Rec.2020.
            rgb = mat3(.6274039, .0690973, .0163914,
                       .3292830, .9195404, .0880133,
                       .0433131, .0113623, .8955953) * rgb;
            vec3 p = pow(clamp(rgb / 10000.0, 0.0, 1.0), vec3(2610.0 / 16384.0));
            rgb = pow((3424.0 / 4096.0 + (2413.0 / 128.0) * p) /
                      (1.0 + (2392.0 / 128.0) * p), vec3(2523.0 / 32.0));
            color = vec4(rgb, color_linear.a);
        } else {
            color = vec4(gamma(color_linear.rgb), color_linear.a);
        }
    }
}
