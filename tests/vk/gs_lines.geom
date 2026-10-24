#version 450
/* Lines to their two end points. */
layout(lines) in;
layout(points, max_vertices = 2) out;
layout(location = 0) flat in uint vindex[];
layout(location = 0) flat out uvec4 color;
void main()
{
    for (int i = 0; i < 2; i++)
    {
        gl_Position = gl_in[i].gl_Position;
        gl_PointSize = 2.0;
        color = uvec4(vindex[i] + 1, 0, 0, 1);
        EmitVertex();
    }
}
