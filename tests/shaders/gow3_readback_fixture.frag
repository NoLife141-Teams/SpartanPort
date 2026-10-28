// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic values only. Alpha identifies the producer frame independently
// of the extraction push constants; RGB exercises sRGB storage conversion.
#version 450
layout(push_constant) uniform Token { uvec4 value; } token;
layout(location=0) out vec4 color;
void main() {
    uint f=token.value.x, s=uint(gl_SampleID);
    uvec2 pixel=uvec2(gl_FragCoord.xy);
    float r=float((pixel.x+3*f+7*s)%29+1)/32.0;
    float g=float((pixel.y+5*f+3*s)%29+1)/32.0;
    float b=s==0 ? 0.002 : 0.18;
    color=vec4(r,g,b,float(16*f+s)/255.0);
    gl_FragDepth=float(32+17*f+71*s+(pixel.x%8)+2*(pixel.y%8))/512.0;
}
