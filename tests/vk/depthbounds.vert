#version 450
/* A rectangle from push constants: x range in NDC and its depth. */
layout(push_constant) uniform P { vec2 x; float z; };
void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    float px = uv.x < 1.0 ? x.x : x.x + (x.y - x.x) * uv.x;
    gl_Position = vec4(px, uv.y * 2.0 - 1.0, z, 1.0);
}
