#!/usr/bin/env python3
"""Shader feature sweep for the Metal Shader Converter: compiles one HLSL snippet per feature (SM 6.0 to 6.8) with dxc
and converts it with tests/msc-bridge/mscconv, then prints ok, WARN (converted, but the converter dropped something:
an accuracy risk unless the bridge keeps such shaders on SPIR-V) or FAILED (the bridge falls back to SPIR-V).
Re-run it on a new converter release; docs/msc-feature-coverage.md has the last results.
  featuresweep.py <dxc> <mscconv> <dir of libmetalirconverter.dylib> <work dir>
(dxc for macOS: ninja -C artifacts/build/dxbcconverter-universal dxc, after scripts/build-dxbcconverter.sh.)"""
import subprocess, sys, os
DXC, MSCCONV, LIBDIR, W = sys.argv[1:5]
P = "RWStructuredBuffer<float4> o : register(u0);\nRWByteAddressBuffer ob : register(u1);\nTexture2D<float4> t : register(t0);\nSamplerState s : register(s0);\nSamplerComparisonState sc : register(s1);\nTexture2D<float> dt : register(t1);\n"
CS = "[numthreads(64,1,1)] void main(uint i : SV_DispatchThreadID, uint gi : SV_GroupIndex) { %s }"
PS = "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { %s }"
cases = [
 ("vs cull distance", "vs_6_0", "struct V{float4 p:SV_Position; float c:SV_CullDistance;}; V main(uint id:SV_VertexID){V v; v.p=id; v.c=id-1.0; return v;}"),
 ("vs clip distance", "vs_6_0", "struct V{float4 p:SV_Position; float c:SV_ClipDistance;}; V main(uint id:SV_VertexID){V v; v.p=id; v.c=id-1.0; return v;}"),
 ("vs rt array index", "vs_6_1", "struct V{float4 p:SV_Position; uint a:SV_RenderTargetArrayIndex;}; V main(uint id:SV_VertexID){V v; v.p=id; v.a=id; return v;}"),
 ("vs viewport index", "vs_6_1", "struct V{float4 p:SV_Position; uint a:SV_ViewportArrayIndex;}; V main(uint id:SV_VertexID){V v; v.p=id; v.a=id; return v;}"),
 ("vs shading rate", "vs_6_4", "struct V{float4 p:SV_Position; uint r:SV_ShadingRate;}; V main(uint id:SV_VertexID){V v; v.p=id; v.r=5; return v;}"),
 ("vs view id", "vs_6_1", "float4 main(uint id:SV_VertexID, uint v:SV_ViewID):SV_Position{return float4(id,v,0,1);}"),
 ("vs start vertex 6.8", "vs_6_8", "float4 main(uint id:SV_VertexID, uint sv:SV_StartVertexLocation, uint si:SV_StartInstanceLocation):SV_Position{return float4(id,sv,si,1);}"),
 ("ps stencil ref", "ps_6_0", "uint main(float4 p:SV_Position):SV_StencilRef{return 3;}"),
 ("ps coverage out", "ps_6_0", "float4 main(float4 p:SV_Position, out uint c:SV_Coverage):SV_Target{c=1; return 1;}"),
 ("ps coverage in", "ps_6_0", "float4 main(float4 p:SV_Position, uint c:SV_Coverage):SV_Target{return c;}"),
 ("ps inner coverage", "ps_6_0", "float4 main(float4 p:SV_Position, uint c:SV_InnerCoverage):SV_Target{return c;}"),
 ("ps depth greater equal", "ps_6_0", "float main(noperspective centroid float4 p:SV_Position, float d:DEPTH):SV_DepthGreaterEqual{return d;}"),
 ("ps depth less equal", "ps_6_0", "float main(noperspective centroid float4 p:SV_Position, float d:DEPTH):SV_DepthLessEqual{return d;}"),
 ("ps depth out", "ps_6_0", "float main(float4 p:SV_Position, float d:DEPTH):SV_Depth{return d;}"),
 ("ps sample index", "ps_6_0", "float4 main(float4 p:SV_Position, uint s:SV_SampleIndex):SV_Target{return s;}"),
 ("ps barycentrics", "ps_6_1", "float4 main(float3 b:SV_Barycentrics):SV_Target{return float4(b,1);}"),
 ("ps attribute at vertex", "ps_6_1", "float4 main(nointerpolation float4 c:COLOR):SV_Target{return GetAttributeAtVertex(c,1);}"),
 ("ps is front face", "ps_6_0", "float4 main(bool f:SV_IsFrontFace):SV_Target{return f;}"),
 ("ps primitive id", "ps_6_0", "float4 main(uint id:SV_PrimitiveID):SV_Target{return id;}"),
 ("ps rt array index in", "ps_6_0", "float4 main(float4 p:SV_Position, uint a:SV_RenderTargetArrayIndex):SV_Target{return a;}"),
 ("ps shading rate in", "ps_6_4", "float4 main(float4 p:SV_Position, uint r:SV_ShadingRate):SV_Target{return r;}"),
 ("ps discard", "ps_6_0", "float4 main(float4 p:SV_Position):SV_Target{if(p.x>4) discard; return 1;}"),
 ("ps ddx coarse/fine", "ps_6_0", "float4 main(float4 p:SV_Position, float2 uv:TEXCOORD0):SV_Target{return float4(ddx_coarse(uv.x),ddy_fine(uv.y),fwidth(uv));}"),
 ("ps eval at sample/centroid/offset", "ps_6_0", "float4 main(float4 p:SV_Position, float2 uv:TEXCOORD0):SV_Target{return float4(EvaluateAttributeAtSample(uv,1),EvaluateAttributeSnapped(uv,int2(1,1)));}"),
 ("ps sample cmp", "ps_6_0", P.replace("\n"," ")+" float4 main(float4 p:SV_Position, float2 uv:TEXCOORD0):SV_Target{return dt.SampleCmp(sc,uv,0.5)+dt.SampleCmpLevelZero(sc,uv,0.5);}"),
 ("ps sample cmp level 6.7", "ps_6_7", P.replace("\n"," ")+" float4 main(float4 p:SV_Position, float2 uv:TEXCOORD0):SV_Target{return dt.SampleCmpLevel(sc,uv,0.5,1.0);}"),
 ("ps calc lod", "ps_6_0", P.replace("\n"," ")+" float4 main(float4 p:SV_Position, float2 uv:TEXCOORD0):SV_Target{return t.CalculateLevelOfDetail(s,uv)+t.CalculateLevelOfDetailUnclamped(s,uv);}"),
 ("ps sample bias/grad/offset", "ps_6_0", P.replace("\n"," ")+" float4 main(float4 p:SV_Position, float2 uv:TEXCOORD0):SV_Target{return t.SampleBias(s,uv,1,int2(1,-1))+t.SampleGrad(s,uv,1,1,int2(2,0));}"),
 ("ps sample clamp + status", "ps_6_0", P.replace("\n"," ")+" float4 main(float4 p:SV_Position, float2 uv:TEXCOORD0):SV_Target{uint st; float4 r=t.Sample(s,uv,int2(0,0),1.0,st); return r+CheckAccessFullyMapped(st);}"),
 ("ps gather cmp/components", "ps_6_0", P.replace("\n"," ")+" float4 main(float4 p:SV_Position, float2 uv:TEXCOORD0):SV_Target{return t.GatherGreen(s,uv,int2(1,1))+dt.GatherCmp(sc,uv,0.5);}"),
 ("ps programmable offset gather", "ps_6_0", P.replace("\n"," ")+" float4 main(float4 p:SV_Position, float2 uv:TEXCOORD0, nointerpolation int2 off:OFF):SV_Target{return t.Gather(s,uv,off);}"),
 ("ps texture MS sample pos", "ps_6_0", "Texture2DMS<float4> m:register(t0); float4 main(float4 p:SV_Position):SV_Target{return float4(m.GetSamplePosition(1),0,0)+m.Load(int2(p.xy),2);}"),
 ("ps quad ops", "ps_6_0", "float4 main(float4 p:SV_Position):SV_Target{return QuadReadAcrossX(p.x)+QuadReadAcrossDiagonal(p.y)+QuadReadLaneAt(p.z,2);}"),
 ("ps quad any/all 6.7", "ps_6_7", "float4 main(float4 p:SV_Position):SV_Target{return QuadAny(p.x>1)+QuadAll(p.y>1);}"),
 ("ps is helper lane 6.6", "ps_6_6", "float4 main(float4 p:SV_Position):SV_Target{return IsHelperLane();}"),
 ("ps uav write + rov", "ps_6_0", "RasterizerOrderedTexture2D<float4> r:register(u1); float4 main(float4 p:SV_Position):SV_Target{r[int2(p.xy)]+=1; return 1;}"),
 ("ps writeMSAA 6.7", "ps_6_7", "RWTexture2DMS<float4,4> m:register(u1); float4 main(float4 p:SV_Position):SV_Target{m.sample[1][int2(p.xy)]=1; return 1;}"),
 ("cs wave basics", "cs_6_0", P+CS % "o[i]=float4(WaveActiveSum(i),WaveReadLaneFirst(i),WaveGetLaneCount(),WaveActiveBallot(i&1).x);"),
 ("cs wave prefix/bits", "cs_6_0", P+CS % "o[i]=float4(WavePrefixSum(i),WavePrefixProduct(i),WaveActiveBitOr(i),WavePrefixCountBits(i&1));"),
 ("cs wave read lane at", "cs_6_0", P+CS % "o[i]=WaveReadLaneAt(float(i),(i+1)&31);"),
 ("cs wave match 6.5", "cs_6_5", P+CS % "o[i]=float4(WaveMatch(i&3));"),
 ("cs wave multiprefix 6.5", "cs_6_5", P+CS % "o[i]=WaveMultiPrefixSum(float(i),WaveMatch(i&3));"),
 ("cs wave size 6.6", "cs_6_6", P+"[WaveSize(32)] "+CS % "o[i]=WaveActiveSum(i);"),
 ("cs wave size range 6.8", "cs_6_8", P+"[WaveSize(16,64,32)] "+CS % "o[i]=WaveActiveSum(i);"),
 ("cs 64-bit wave", "cs_6_0", P+CS % "uint64_t v=i; ob.Store(i*8, WaveActiveSum(v));"),
 ("cs int64 math", "cs_6_0", P+CS % "uint64_t v=(uint64_t)i*0x100000001ull; v=v/(i+3)+(v>>7); ob.Store(i*8,v);"),
 ("cs double math", "cs_6_0", P+CS % "double d=i; d=fma(d,d,1.0)/(d+0.5); ob.Store(i*8,d);"),
 ("cs splitdouble only", "cs_6_0", P+CS % "uint lo,hi; asuint((double)i*1.5,lo,hi); ob.Store2(i*8,uint2(lo,hi));"),
 ("cs double arith only", "cs_6_0", P+CS % "double d=(double)i; d=d*d+0.5; o[i]=float4((float)d,0,0,0);"),
 ("cs 16-bit types", "cs_6_2 -enable-16bit-types", P+CS % "half h=(half)i; int16_t s=(int16_t)i; o[i]=float4(h*h,s,0,0);"),
 ("cs dot4add 6.4", "cs_6_4 -enable-16bit-types", P+CS % "o[i]=float4(dot4add_u8packed(i,i*3,1u),dot4add_i8packed(i,i,0),dot2add(half2(1,2),half2(i,1),0.0),0);"),
 ("cs pack/unpack 6.6", "cs_6_6 -enable-16bit-types", P+CS % "int8_t4_packed p=pack_s8((int16_t4)int4(i,1,2,3)); int16_t4 u=unpack_s8s16(p); o[i]=float4(u.x,u.y,(uint)pack_clamp_u8((int16_t4)int4(300,1,2,3)),0);"),
 ("cs bit ops", "cs_6_0", P+CS % "o[i]=float4(countbits(i),firstbithigh(i),firstbitlow(i),reversebits(i));"),
 ("cs msad4 / f16 conv", "cs_6_0", P+CS % "o[i]=float4(msad4(i,uint2(i,3),uint4(0,1,2,3)).x,f16tof32(i),f32tof16(1.5),0);"),
 ("cs groupshared atomics", "cs_6_0", "groupshared uint g; "+P+CS % "uint v; InterlockedAdd(g,1,v); InterlockedMax(g,i); GroupMemoryBarrierWithGroupSync(); o[i]=g;"),
 ("cs 64-bit atomics 6.6", "cs_6_6", P+CS % "ob.InterlockedAdd64(0,(uint64_t)i);"),
 ("cs 64-bit groupshared atomics 6.6", "cs_6_6", "groupshared uint64_t g; "+P+CS % "InterlockedAdd(g,(uint64_t)i); GroupMemoryBarrierWithGroupSync(); ob.Store(i*8,g);"),
 ("cs float atomic cmpxchg 6.6", "cs_6_6", "RWStructuredBuffer<float> f:register(u2); "+P+CS % "float orig; InterlockedCompareExchangeFloatBitwise(f[0],0.0,(float)i,orig);"),
 ("cs typed uav atomics", "cs_6_0", "RWTexture2D<uint> u:register(u2); "+P+CS % "InterlockedOr(u[int2(i,0)],1);"),
 ("cs heap indexing 6.6", "cs_6_6", CS % "RWStructuredBuffer<float4> b=ResourceDescriptorHeap[i&7]; Texture2D<float4> tx=ResourceDescriptorHeap[8+(i&3)]; SamplerState ss=SamplerDescriptorHeap[0]; b[i]=tx.SampleLevel(ss,float2(0.5,0.5),0);"),
 ("cs nonuniform index", "cs_6_0", "Texture2D<float4> ts[8]:register(t0); SamplerState s:register(s0); RWStructuredBuffer<float4> o:register(u0); "+CS % "o[i]=ts[NonUniformResourceIndex(i&7)].SampleLevel(s,float2(0.5,0.5),0);"),
 ("cs texture gather raw 6.7", "cs_6_7", "Texture2D<uint> tu:register(t0); SamplerState s:register(s0); RWStructuredBuffer<uint4> o:register(u0); "+CS % "o[i]=tu.GatherRaw(s,float2(0.5,0.5));"),
 ("cs derivatives 6.6", "cs_6_6", P+CS % "o[i]=t.Sample(s,float2(i*0.1,0.5))+ddx(float(i));"),
 ("cs getdimensions", "cs_6_0", "Texture2DArray<float4> ta:register(t2); Texture3D<float4> t3:register(t3); "+P+CS % "uint w,h,e,l; ta.GetDimensions(1,w,h,e,l); uint a,b,c; t3.GetDimensions(a,b,c); uint n; ob.GetDimensions(n); o[i]=float4(w+h+e+l,a+b+c,n,0);"),
 ("cs sampler feedback", "cs_6_5", "FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> f:register(u2); "+P+CS % "f.WriteSamplerFeedbackLevel(t,s,float2(0.5,0.5),0);"),
 ("cs barrier 6.8", "cs_6_8", P+CS % "o[i]=1; Barrier(UAV_MEMORY, DEVICE_SCOPE); o[i+1]+=1;"),
 ("cs isnan/denorm double", "cs_6_0", P+CS % "double d=asdouble(i,0); ob.Store(i*4,(uint)isnan(d)+(uint)isinf((float)i));"),
 ("ms basic", "ms_6_5", "struct V{float4 p:SV_Position;}; [outputtopology(\"triangle\")][numthreads(32,1,1)] void main(uint i:SV_GroupThreadID, out vertices V v[3], out indices uint3 t[1]){SetMeshOutputCounts(3,1); if(i<3) v[i].p=i; if(i==0) t[0]=uint3(0,1,2);}"),
 ("ms cull primitive", "ms_6_5", "struct V{float4 p:SV_Position;}; struct P{bool c:SV_CullPrimitive;}; [outputtopology(\"triangle\")][numthreads(32,1,1)] void main(uint i:SV_GroupThreadID, out vertices V v[3], out indices uint3 t[1], out primitives P pr[1]){SetMeshOutputCounts(3,1); if(i<3) v[i].p=i; if(i==0){t[0]=uint3(0,1,2); pr[0].c=true;}}"),
 ("ms primitive shading rate", "ms_6_5", "struct V{float4 p:SV_Position;}; struct P{uint r:SV_ShadingRate;}; [outputtopology(\"triangle\")][numthreads(32,1,1)] void main(uint i:SV_GroupThreadID, out vertices V v[3], out indices uint3 t[1], out primitives P pr[1]){SetMeshOutputCounts(3,1); if(i<3) v[i].p=i; if(i==0){t[0]=uint3(0,1,2); pr[0].r=5;}}"),
 ("ms cull distance", "ms_6_5", "struct V{float4 p:SV_Position; float c:SV_CullDistance;}; [outputtopology(\"triangle\")][numthreads(32,1,1)] void main(uint i:SV_GroupThreadID, out vertices V v[3], out indices uint3 t[1]){SetMeshOutputCounts(3,1); if(i<3){v[i].p=i; v[i].c=-1;} if(i==0) t[0]=uint3(0,1,2);}"),
 ("ms rt array index per prim", "ms_6_5", "struct V{float4 p:SV_Position;}; struct P{uint a:SV_RenderTargetArrayIndex; uint vp:SV_ViewportArrayIndex;}; [outputtopology(\"triangle\")][numthreads(32,1,1)] void main(uint i:SV_GroupThreadID, out vertices V v[3], out indices uint3 t[1], out primitives P pr[1]){SetMeshOutputCounts(3,1); if(i<3) v[i].p=i; if(i==0){t[0]=uint3(0,1,2); pr[0].a=1; pr[0].vp=1;}}"),
]
for name, target, src in cases:
    tag = name.replace(' ', '_').replace('/', '_').replace('+', '')
    hl, dx, ml = f"{W}/{tag}.hlsl", f"{W}/{tag}.dxil", f"{W}/{tag}.metallib"
    open(hl, "w").write(src)
    parts = target.split()
    r = subprocess.run([DXC, "-T", parts[0], *parts[1:], "-E", "main", "-Fo", dx, hl], capture_output=True, text=True)
    if r.returncode:
        print(f"{name:36} DXC FAILED: {r.stderr.strip().splitlines()[0][:110] if r.stderr.strip() else ''}")
        continue
    r = subprocess.run([MSCCONV, dx, ml], capture_output=True, text=True, env={**os.environ, "DYLD_LIBRARY_PATH": LIBDIR})
    out = (r.stdout + r.stderr).strip().splitlines()
    msgs = [l for l in out if "top-level resources" not in l]
    status = "FAILED" if r.returncode else ("WARN" if msgs else "ok")
    print(f"{name:36} {status:6} {' | '.join(m[:120] for m in msgs)}")
