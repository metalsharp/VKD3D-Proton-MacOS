#version 450
/* Lines with adjacency to the points of the line, colored by the four input vertexes. */
layout(lines_adjacency) in;
layout(points, max_vertices = 2) out;
layout(location = 0) flat in uint vindex[];
layout(location = 0) flat out uvec4 color;
void main()
{
    for (int i = 1; i < 3; i++)
    {
        gl_Position = gl_in[i].gl_Position;
        gl_PointSize = 2.0;
        color = uvec4(vindex[0] + 1, vindex[1] + 1, vindex[2] + 1, vindex[3] + 1);
        EmitVertex();
    }
}
