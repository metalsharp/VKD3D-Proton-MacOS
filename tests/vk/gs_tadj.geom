#version 450
/* Triangles with adjacency to their triangle, colored by input vertexes 0, 1, 3 and 5. */
layout(triangles_adjacency) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) flat in uint vindex[];
layout(location = 0) flat out uvec4 color;
void main()
{
    for (int i = 0; i < 3; i++)
    {
        gl_Position = gl_in[2 * i].gl_Position;
        color = uvec4(vindex[0] + 1, vindex[1] + 1, vindex[3] + 1, vindex[5] + 1);
        EmitVertex();
    }
}
