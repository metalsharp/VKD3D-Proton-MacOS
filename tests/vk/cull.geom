#version 460
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in float vcull[];
out float gl_CullDistance[1];
void main()
{
    for (int i = 0; i < 3; i++)
    {
        gl_Position = gl_in[i].gl_Position;
        gl_CullDistance[0] = vcull[i];
        EmitVertex();
    }
}
