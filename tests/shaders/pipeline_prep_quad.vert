// SPDX-License-Identifier: GPL-2.0-or-later
#version 460
void main() {
    const vec2 points[4] = vec2[4](vec2(-1,-1), vec2(-1,1), vec2(1,1), vec2(1,-1));
    gl_Position = vec4(points[gl_VertexIndex], 0, 1);
}
