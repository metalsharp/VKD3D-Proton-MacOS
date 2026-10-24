#version 460
#extension GL_EXT_multiview : require
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) flat in uint vview[];
layout(location = 0) flat out uint oview;
void main()
{
    for (int i = 0; i < 3; i++)
    {
        gl_Position = gl_in[i].gl_Position;
        oview = vview[i] | (gl_ViewIndex << 2);
        EmitVertex();
    }
    EndPrimitive();
}
