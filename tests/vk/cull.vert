#version 460
layout(location = 0) out float vcull;
out float gl_CullDistance[1];
// Quads over the thirds of the target; instance 0's vertices all have a negative cull distance (culled),
// instance 1's left ones (drawn: all of it), instance 2's none.
void main()
{
    const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
    vec2 v = c[gl_VertexIndex];
    gl_Position = vec4((float(gl_InstanceIndex) + v.x) / 3.0 * 2.0 - 1.0, v.y * 2.0 - 1.0, 0.0, 1.0);
    float d = gl_InstanceIndex == 0 ? -1.0 : gl_InstanceIndex == 1 ? (v.x == 0.0 ? -1.0 : 1.0) : 1.0;
    gl_CullDistance[0] = d;
    vcull = d;
}
