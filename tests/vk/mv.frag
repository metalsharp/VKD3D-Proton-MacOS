#version 460
#extension GL_EXT_multiview : require
layout(location = 0) flat in uint vview;
layout(location = 0) out uvec4 o;
void main() { o = uvec4(vview + 1u, gl_ViewIndex + 1u, 0u, 255u); }
