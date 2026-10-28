// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
layout(location=0) out vec4 sample_metadata;
void main() {
#if SAMPLES == 2
    sample_metadata = vec4(gl_SamplePosition, gl_FragCoord.z, float(gl_SampleID));
#else
    sample_metadata = vec4(.5, .5, gl_FragCoord.z, 0);
#endif
}
