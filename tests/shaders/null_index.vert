// SPDX-License-Identifier: GPL-2.0-or-later
#version 460
layout(set = 0, binding = 0, std430) buffer Output { uint words[]; } result;
void main() {
    result.words[gl_InstanceIndex] = uint(gl_VertexIndex);
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
