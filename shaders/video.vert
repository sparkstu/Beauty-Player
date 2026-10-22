#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;

layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform buf {
    vec4 scale;    // xy = letterbox scale, z = display-matrix rotation, w = mirror
    vec4 params;   // x = 0 YUV, 1 RGB passthrough, 2 RGB video
    vec4 extra;
    vec4 color;
} ubuf;

void main() {
    // The display matrix (phone videos) is applied to the *sampling* coordinate,
    // so the whole chain downstream already works in display orientation. The
    // mirror flips the displayed image, so it is applied before the rotation.
    vec2 d = uv;
    if (ubuf.scale.w > 0.5) d.x = 1.0 - d.x;
    const int r = int(ubuf.scale.z + 0.5);
    if (r == 1) {          // 90 degrees clockwise
        d = vec2(d.y, 1.0 - d.x);
    } else if (r == 2) {   // 180 degrees
        d = vec2(1.0 - d.x, 1.0 - d.y);
    } else if (r == 3) {   // 270 degrees clockwise (= 90 counter-clockwise)
        d = vec2(1.0 - d.y, d.x);
    }
    v_uv = d;
    gl_Position = vec4(position * ubuf.scale.xy, 0.0, 1.0);
}
