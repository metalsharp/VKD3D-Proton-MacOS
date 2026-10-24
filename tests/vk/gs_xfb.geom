#version 450
/* Stream output from a geometry shader: point p, invocation i emits a strip of 2 + p % 3 vertexes (0, 1 or 2
 * triangles), capturing (primitive ID, vertex in strip, invocation, 1) and the position. */
layout(points, invocations = 2) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 1, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 16) out vec4 o;
layout(xfb_buffer = 1, xfb_offset = 0, xfb_stride = 16) out gl_PerVertex { vec4 gl_Position; };
layout(location = 0) flat out uvec4 color;
void main()
{
    uint n = 2 + gl_PrimitiveIDIn % 3;
    for (uint k = 0; k < n; k++)
    {
        gl_Position = vec4(-1.0 + 0.5 * float(k), -1.0 + 0.25 * float(gl_PrimitiveIDIn + (k & 1)), 0.0, 1.0);
        o = vec4(gl_PrimitiveIDIn, k, gl_InvocationID, 1.0);
        color = uvec4(1);
        EmitVertex();
    }
}
