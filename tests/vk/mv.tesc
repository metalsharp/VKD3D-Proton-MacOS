#version 460
#extension GL_EXT_multiview : require
layout(vertices = 3) out;
layout(location = 0) flat in uint vview[];
layout(location = 0) flat out uint tview[];
void main()
{
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    tview[gl_InvocationID] = vview[gl_InvocationID] | (gl_ViewIndex << 2);
    if (gl_InvocationID == 0)
    {
        gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = gl_TessLevelOuter[2] = 1.0;
        gl_TessLevelInner[0] = 1.0;
    }
}
