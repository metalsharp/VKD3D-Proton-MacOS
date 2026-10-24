#version 460
#extension GL_EXT_multiview : require
layout(location = 0) flat out uint vview;
void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vview = gl_ViewIndex;
}
