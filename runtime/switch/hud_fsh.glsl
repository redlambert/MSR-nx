// MSR-nx: single-screen mode, part of the bottom screen over the top screen (main_nx.c present).
// look.x picks how:
//  0: a HUD readout. Its dark background is see-through; lines, icons and text get more opaque the
//     brighter they are, and bright text is fully opaque. look.y, when above 0, fades it.
//  1: text (the prologue's lines, the title's prompt), keyed as msr2ds's compositor keys it: below
//     0.45 brightness nothing, above 0.65 all, after look.z's gain (the title's prompt pulses, and its
//     dim phases would vanish); look.y the opacity.
//  2: the dark card behind the text, look.y its opacity.
//  3: the minimap: look.y its opacity, look.z a brightness gain.
#version 460
layout (std140, binding = 0) uniform Rects {
    vec4 dst;
    vec4 src;
    vec4 look;
};
layout (binding = 0) uniform sampler2D bottom;
layout (location = 0) in vec2 v_uv;
layout (location = 0) out vec4 o;
void main() {
    if (look.x > 1.5) {
        o = vec4(0.01, 0.035, 0.07, look.y);
        return;
    }
    vec3 c = texture(bottom, v_uv).rgb;
    if (look.x > 2.5) {
        o = vec4(min(c * look.z, vec3(1.0)), look.y);
        return;
    }
    if (look.x > 0.5) {
        c = min(c * look.z, vec3(1.0));
        float m = max(c.r, max(c.g, c.b));
        o = vec4(c, smoothstep(0.45, 0.65, m) * look.y);
        return;
    }
    float m = max(c.r, max(c.g, c.b));
    float a = clamp((m - 0.25) / 0.45, 0.0, 1.0);
    o = vec4(c, (0.3 + 0.7 * a) * (look.y > 0.0 ? look.y : 1.0));   // look.y: a fade (the selected beam's)
}
