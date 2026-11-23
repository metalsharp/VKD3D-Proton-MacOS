/*
 * d3d11_alu.c — D3D11 shader arithmetic with D3D-defined results (NaN and infinity handling, saturating
 * conversions, half-float packing, bit operations), checked in a compute shader. Inputs come from a
 * constant buffer so the HLSL compiler cannot fold them.
 *
 *   d3d11_alu.exe            runs every check and prints the ones that differ from D3D's result
 *
 * Build: x86_64-w64-mingw32-clang -O2 d3d11_alu.c -o d3d11_alu.exe -ld3d11 -ld3dcompiler_47 -luuid
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

/* c.x = NaN, c.y = +inf, c.z = -2.7, c.w = 1.5; d = 0, -1, 3e9, 0.25 */
static const char shader[] =
    "cbuffer k : register(b0) { uint4 c; float4 d; };\n"
    "RWStructuredBuffer<uint> o : register(u0);\n"
    "[numthreads(1, 1, 1)] void main() {\n"
    "    float nan = asfloat(c.x), inf = asfloat(c.y), m27 = asfloat(c.z), f15 = asfloat(c.w);\n"
    "    float zero = d.x, m1 = d.y, big = d.z, q = d.w;\n"
    "    uint i = 0;\n"
    "    o[i++] = asuint(saturate(nan));\n"            /* 0: D3D saturate(NaN) = 0 */
    "    o[i++] = asuint(max(nan, 1.0 + zero));\n"     /* 1: max(NaN, 1) = 1 */
    "    o[i++] = asuint(min(nan, 1.0 + zero));\n"     /* 2: min(NaN, 1) = 1 */
    "    o[i++] = (uint)m1;\n"                         /* 3: ftou(-1) = 0 */
    "    o[i++] = (uint)nan;\n"                        /* 4: ftou(NaN) = 0 */
    "    o[i++] = (uint)(int)big;\n"                   /* 5: ftoi(3e9) = INT_MAX */
    "    o[i++] = (uint)(int)nan;\n"                   /* 6: ftoi(NaN) = 0 */
    "    o[i++] = (uint)(int)m27;\n"                   /* 7: ftoi(-2.7) = -2 */
    "    o[i++] = f32tof16(f15);\n"                    /* 8: 0x3e00 */
    "    o[i++] = asuint(f16tof32(0x3e00 + (uint)zero));\n"  /* 9: 1.5 */
    "    o[i++] = f32tof16(inf);\n"                    /* 10: 0x7c00 */
    "    o[i++] = f32tof16(65520.0 + zero);\n"         /* 11: rounds to inf, 0x7c00 */
    "    o[i++] = countbits(0xf0f0u + (uint)zero);\n"  /* 12: 8 */
    "    o[i++] = firstbithigh(0x80u + (uint)zero);\n" /* 13: 7 */
    "    o[i++] = firstbitlow(0x80u + (uint)zero);\n"  /* 14: 7 */
    "    o[i++] = reversebits(1u + (uint)zero);\n"     /* 15: 0x80000000 */
    "    o[i++] = asuint(round(2.5 + zero));\n"        /* 16: round_ne: 2.0 */
    "    o[i++] = asuint(frac(-q));\n"                 /* 17: 0.75 */
    "    o[i++] = asuint(rsqrt(4.0 + zero));\n"        /* 18: 0.5 */
    "    o[i++] = isnan(nan) ? 1 : 0;\n"               /* 19: 1 */
    "    o[i++] = isinf(inf) ? 1 : 0;\n"               /* 20: 1 */
    "    o[i++] = asuint(1.0 / zero);\n"               /* 21: +inf */
    "    o[i++] = asuint(log(zero));\n"                /* 22: -inf */
    "    o[i++] = asuint(exp(-inf));\n"                /* 23: 0 */
    "    o[i++] = asuint(saturate(sqrt(m1)));\n"       /* 24: sqrt(-1) = NaN, saturate = 0 */
    "    o[i++] = firstbithigh(0u + (uint)zero);\n"    /* 25: no bit: 0xffffffff */
    "    o[i++] = asuint(pow(zero, zero));\n"          /* 26: D3D pow(0, 0) = NaN? exp2(0 * log2(0)) = exp2(NaN) */
    "    o[i++] = (uint)firstbithigh(-2 + (int)zero);\n" /* 27: signed: first bit differing from the sign: 0 */
    "    o[i++] = asuint(f16tof32(0x7e00 + (uint)zero)) & 0x7fc00000;\n" /* 28: half NaN -> float NaN 0x7fc00000 */
    "    o[i++] = asuint(normalize(float3(zero, zero, zero)).x) & 0x7f800000;\n" /* 29: NaN exponent bits */
    "    o[i++] = asuint(sqrt(m1));\n"                 /* 30: sqrt(-1) = NaN */
    "    o[i++] = asuint(sqrt(-q)) & 0x7f800000;\n"    /* 31: sqrt(-0.25): NaN exponent bits */
    "    o[i++] = asuint(rsqrt(m1)) & 0x7f800000;\n"   /* 32: rsqrt(-1): NaN exponent bits */
    "    o[i++] = asuint(log(m1)) & 0x7f800000;\n"     /* 33: log(-1): NaN exponent bits */
    "}\n";

struct check { const char *what; unsigned want; int nan_ok; };
static const struct check checks[] = {
    { "saturate(NaN)", 0x00000000 }, { "max(NaN, 1)", 0x3f800000 }, { "min(NaN, 1)", 0x3f800000 },
    { "(uint)-1.0", 0 }, { "(uint)NaN", 0 }, { "(int)3e9", 0x7fffffff }, { "(int)NaN", 0 }, { "(int)-2.7", 0xfffffffe },
    { "f32tof16(1.5)", 0x3e00 }, { "f16tof32(0x3e00)", 0x3fc00000 }, { "f32tof16(inf)", 0x7c00 }, { "f32tof16(65520)", 0x7c00 },
    { "countbits(0xf0f0)", 8 }, { "firstbithigh(0x80)", 7 }, { "firstbitlow(0x80)", 7 }, { "reversebits(1)", 0x80000000 },
    { "round(2.5)", 0x40000000 }, { "frac(-0.25)", 0x3f400000 }, { "rsqrt(4)", 0x3f000000 },
    { "isnan(NaN)", 1 }, { "isinf(inf)", 1 }, { "1/0", 0x7f800000 }, { "log(0)", 0xff800000 }, { "exp(-inf)", 0 },
    { "saturate(sqrt(-1))", 0 }, { "firstbithigh(0)", 0xffffffff }, { "pow(0, 0)", 0, 1 },
    { "firstbithigh(-2) signed", 0 }, { "f16tof32(half NaN)", 0x7fc00000 }, { "normalize(0) is NaN", 0x7f800000 },
    { "sqrt(-1)", 0x7fc00000, 1 }, { "sqrt(-0.25) is NaN", 0x7f800000 }, { "rsqrt(-1) is NaN", 0x7f800000 }, { "log(-1) is NaN", 0x7f800000 },
};

int main(void) {
    ID3D11Device *dev; ID3D11DeviceContext *ctx; D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ID3DBlob *blob = NULL, *err = NULL; ID3D11ComputeShader *cs; ID3D11Buffer *cb, *out, *rb; ID3D11UnorderedAccessView *uav;
    unsigned n = sizeof(checks) / sizeof(checks[0]), bad = 0;
    unsigned kdata[8] = { 0x7fc00000, 0x7f800000, 0xc02ccccd, 0x3fc00000 };
    float *kf = (float *)kdata; D3D11_MAPPED_SUBRESOURCE m;
    kf[4] = 0.0f; kf[5] = -1.0f; kf[6] = 3e9f; kf[7] = 0.25f;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx))) { printf("FAIL device\n"); return 1; }
    if (FAILED(D3DCompile(shader, strlen(shader), NULL, NULL, NULL, "main", "cs_5_0", 0, 0, &blob, &err))) {
        printf("FAIL compile: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 1;
    }
    ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), NULL, &cs);
    { D3D11_BUFFER_DESC d = { 32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 }; D3D11_SUBRESOURCE_DATA s = { kdata, 0, 0 }; ID3D11Device_CreateBuffer(dev, &d, &s, &cb); }
    { D3D11_BUFFER_DESC d = { 64 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4 }; ID3D11Device_CreateBuffer(dev, &d, NULL, &out); }
    { D3D11_BUFFER_DESC d = { 64 * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 }; ID3D11Device_CreateBuffer(dev, &d, NULL, &rb); }
    ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)out, NULL, &uav);
    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 1, 1, 1);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)rb, (ID3D11Resource *)out);
    ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)rb, 0, D3D11_MAP_READ, 0, &m);
    for (unsigned i = 0; i < n; i++) {
        unsigned got = ((unsigned *)m.pData)[i];
        int ok = got == checks[i].want || (checks[i].nan_ok && (got & 0x7f800000) == 0x7f800000 && (got & 0x7fffff));
        if (!ok) bad++;
        printf("%s %-26s got %08x want %08x\n", ok ? "ok  " : "BAD ", checks[i].what, got, checks[i].want);
    }
    printf("%u of %u differ from D3D\n", bad, n);
    return bad ? 1 : 0;
}
