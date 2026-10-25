#version 450
/* Vertices from a table selected by push constants; the vertex index goes to the geometry shader. */
layout(push_constant) uniform P { uint table; };
layout(location = 0) flat out uint vindex;
const vec2 points[4] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(-0.5, 0.5), vec2(0.5, 0.5));
const vec2 halves[6] = vec2[](vec2(-1, -1), vec2(0, -1), vec2(-1, 1), vec2(0, -1), vec2(1, -1), vec2(0, 1));
const vec2 strip[4] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, 1));
/* A strip with adjacency: the strip's vertexes at even indexes, adjacency vertexes in between. */
const vec2 adjacency[8] = vec2[](vec2(-1, -1), vec2(0, 0), vec2(1, -1), vec2(0, 0), vec2(-1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 0));
void main()
{
    vec2 p = table == 0 ? points[gl_VertexIndex & 3] : table == 1 ? halves[gl_VertexIndex % 6] :
             table == 2 ? strip[gl_VertexIndex & 3] : adjacency[gl_VertexIndex & 7];
    gl_Position = vec4(p, 0.0, 1.0);
    vindex = gl_VertexIndex;
}
