#version 460
out float gl_ClipDistance[1];
out float gl_CullDistance[1];
// Both: one quad over the target with its top half clipped, and a cull distance negative only at its left vertices.
void main()
{
    const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    vec2 v = c[gl_VertexIndex];
    gl_Position = vec4(v * 2.0 - 1.0, 0.0, 1.0);
    gl_ClipDistance[0] = v.y == 0.0 ? 1.0 : -1.0;
    gl_CullDistance[0] = v.x == 0.0 ? -1.0 : 1.0;
}
