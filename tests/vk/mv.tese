#version 460
#extension GL_EXT_multiview : require
layout(triangles, equal_spacing, ccw) in;
layout(location = 0) flat in uint tview[];
layout(location = 0) flat out uint oview;
void main()
{
    gl_Position = gl_TessCoord.x * gl_in[0].gl_Position + gl_TessCoord.y * gl_in[1].gl_Position + gl_TessCoord.z * gl_in[2].gl_Position;
    oview = tview[0] | (gl_ViewIndex << 4);
}
