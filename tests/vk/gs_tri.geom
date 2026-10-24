#version 450
/* Triangles through, colored by the primitive ID and the input vertex indexes (in input order). */
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) flat in uint vindex[];
layout(location = 0) flat out uvec4 color;
void main()
{
    for (int i = 0; i < 3; i++)
    {
        gl_Position = gl_in[i].gl_Position;
        color = uvec4(gl_PrimitiveIDIn + 1, vindex[0] + 1, vindex[1] + 1, vindex[2] + 1);
        EmitVertex();
    }
}
