// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(push_constant) uniform Geometry { mat4 vp; vec4 corners[4]; } pc;
layout(constant_id=0) const bool convert_minus_depth=false;
void main() {
    gl_Position = pc.vp * pc.corners[gl_VertexIndex];
    if (convert_minus_depth) gl_Position.z=(gl_Position.z+gl_Position.w)*.5;
}
