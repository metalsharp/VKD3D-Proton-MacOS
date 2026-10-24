#version 450
/* Captures the vertex attribute (tagged with the instance) and the vertex and instance indexes into two
 * transform feedback buffers, 0 and 2, and rasterizes the attribute's position. */
layout(location = 0) in vec4 a;
layout(location = 0, xfb_buffer = 0, xfb_offset = 0, xfb_stride = 16) out vec4 o0;
layout(location = 1, xfb_buffer = 2, xfb_offset = 0, xfb_stride = 8) flat out uvec2 o1;
void main()
{
    gl_Position = vec4(a.xy, 0.0, 1.0);
    o0 = a + vec4(0.0, 0.0, 0.0, 100.0 * float(gl_InstanceIndex));
    o1 = uvec2(gl_VertexIndex, gl_InstanceIndex);
}
