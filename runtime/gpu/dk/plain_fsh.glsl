// MSR-nx: GPU test "Plain pixels" (port menu). Every draw colored as cheaply as a 3DS draw can be:
// its first texture (when the draw uses one) times its vertex color. No fragment lighting, no
// texture combiners, no alpha test, no depth value written. Pictures look wrong on purpose; the
// frame rate against "Normal" shows what pica_fsh.glsl's work costs.
#version 460

layout (location = 0) in vec4 v_color;
layout (location = 1) in vec4 v_uv01;

layout (location = 0) out vec4 out_color;

layout (binding = 0) uniform sampler2D tex0;

layout (std140, binding = 1) uniform FragmentState {
    uvec4 regs[88];   // PICA registers 0x80..0x1DF
    vec4 depth;
};

void main() {
    vec4 c = clamp(v_color, 0.0, 1.0);
    if ((regs[0].x & 1u) != 0u)
        c *= texture(tex0, v_uv01.xy);
    out_color = c;
}
