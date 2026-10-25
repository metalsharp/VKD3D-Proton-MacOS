#version 450
/* Points to quads (a 4-vertex strip each), colored by the point's vertex index; two invocations, the second
 * offset to the right (and colored by its invocation) when the push constant asks for it. */
layout(points, invocations = 2) in;
layout(triangle_strip, max_vertices = 4) out;
layout(push_constant) uniform P { uint table; uint instanced; };
layout(location = 0) flat in uint vindex[];
layout(location = 0) flat out uvec4 color;
void main()
{
    if (gl_InvocationID == 1 && instanced == 0)
        return;
    vec2 c = gl_in[0].gl_Position.xy + vec2(gl_InvocationID == 1 ? 0.5 : 0.0, 0.0);
    for (int i = 0; i < 4; i++)
    {
        vec2 o = vec2((i & 1) != 0 ? 0.25 : -0.25, (i & 2) != 0 ? 0.25 : -0.25);
        gl_Position = vec4(c + o, 0.0, 1.0);
        color = uvec4(vindex[0] + 1, gl_InvocationID + 1, 0, 1);
        EmitVertex();
    }
}
