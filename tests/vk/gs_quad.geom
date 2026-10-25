#version 450
/* One input triangle to a fullscreen quad as a 4-vertex strip, both triangles clockwise in the framebuffer
 * per the strip rules: drawn with front faces culled, both must remain. */
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) flat out uvec4 color;
void main()
{
    const vec2 q[4] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, 1));
    for (int i = 0; i < 4; i++)
    {
        gl_Position = vec4(q[i], 0.0, 1.0);
        color = uvec4(1, 1, 1, 1);
        EmitVertex();
    }
}
