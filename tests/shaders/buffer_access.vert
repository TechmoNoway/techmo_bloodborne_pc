// SPDX-License-Identifier: GPL-2.0-or-later
#version 460
layout(location=0) in uint value;
layout(set=0,binding=0,std430) buffer Output { uint values[]; } result;
void main() {
    result.values[gl_VertexIndex]=value;
    gl_Position=vec4(0.0,0.0,0.0,1.0);
}
