// Tessellated triangles with no vertex buffer, for tess_stress.c: MoltenVK runs the vertex and hull stages as compute
// work whose outputs go to temporary buffers sized by the draw.
struct VSOut { float4 pos : POS; };
struct HSConst { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
struct DSOut { float4 pos : SV_Position; };

#ifdef VS
VSOut main(uint id : SV_VertexID)
{
    VSOut o;
    float2 p = float2((id % 3) == 1, (id % 3) == 2) * 0.01 + float2((id / 3) % 200, (id / 600) % 200) * 0.01 - 1.0;
    o.pos = float4(p, 0.5, 1.0);
    return o;
}
#endif
#ifdef HS
HSConst patch_const(InputPatch<VSOut, 3> ip)
{
    HSConst c;
    c.edges[0] = c.edges[1] = c.edges[2] = 4.0;
    c.inside = 4.0;
    return c;
}
[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")] [outputcontrolpoints(3)]
[patchconstantfunc("patch_const")]
VSOut main(InputPatch<VSOut, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }
#endif
#ifdef DS
[domain("tri")]
DSOut main(HSConst c, float3 b : SV_DomainLocation, const OutputPatch<VSOut, 3> p)
{
    DSOut o;
    o.pos = p[0].pos * b.x + p[1].pos * b.y + p[2].pos * b.z;
    return o;
}
#endif
#ifdef PS
float4 main() : SV_Target { return float4(1, 0.5, 0, 1); }
#endif
