// fp8exh: SPIRV-Cross's FP8 E4M3 conversions (spvEncodeFP8E4M3, the rounding cooperative matrices use) against a CPU
// reference, OCP E4M3 rounded to nearest even: every half and 4 million floats (values next to every FP8 value and
// halfway between them, then a sweep of all exponents), saturating and not, in Metal's safe and fast math.
//   tests/coopmat/msl-helpers.py Float8 > fp8.metal
//   clang -O1 -fobjc-arc -framework Metal -framework Foundation tests/coopmat/fp8exh.m -o fp8exh && ./fp8exh fp8.metal
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
// Reference: OCP E4M3, round to nearest even; NaN -> 0xff; inf -> s|0x7f; >= 464 -> s|0x7f or s|0x7e saturated.
static double val(int c) { int e = (c >> 3) & 15, m = c & 7; double v = e ? ldexp(1 + m / 8.0, e - 7) : ldexp(m / 8.0, -6); return (c & 0x80) ? -v : v; }
static int ref(double x, int sat) {
  if (isnan(x)) return 0xff;
  int s = signbit(x) ? 0x80 : 0; double a = fabs(x);
  if (isinf(a)) return s | 0x7f;
  if (a >= 464) return s | (sat ? 0x7e : 0x7f);
  int best = 0; double bd = 1e30;
  for (int c = 0; c < 0x7f; c++) { double d = fabs(val(c) - a); if (d < bd || (d == bd && !(c & 1))) { bd = d; best = c; } }
  return s | best;
}
int main(int argc, char **argv) {
  int failed = 0;
  @autoreleasepool {
    if (argc < 2) { printf("usage: fp8exh <FP8 helper MSL from msl-helpers.py Float8>\n"); return 1; }
    NSString *helpers = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
    if (!helpers) { printf("cannot read %s\n", argv[1]); return 1; }
    NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n%@\n"
      "kernel void k(device const uint *in [[buffer(0)]], device uchar4 *out [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
      "  float f = as_type<float>(in[i]); half h = as_type<half>(ushort(i & 0xffff));\n"
      "  out[i] = uchar4(spvEncodeFP8E4M3(f, false), spvEncodeFP8E4M3(f, true), spvEncodeFP8E4M3(h, false), spvEncodeFP8E4M3(h, true));\n"
      "}\n", helpers];
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue];
    const int N = 1 << 22;
    for (int fast = 0; fast < 2; fast++) {
      MTLCompileOptions *o = [MTLCompileOptions new];
      o.mathMode = fast ? MTLMathModeFast : MTLMathModeSafe;
      NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:o error:&e];
      if (!lib) { printf("%s\n", e.localizedDescription.UTF8String); return 1; }
      id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&e];
      id<MTLBuffer> in = [dev newBufferWithLength:N * 4 options:0], out = [dev newBufferWithLength:N * 4 options:0];
      uint32_t *f = in.contents;
      for (int i = 0; i < N; i++) {
        // floats near FP8 values and ties, plus a sweep of every exponent
        if (i < 65536) { uint32_t c = i & 0xff, k = i >> 8; float v = (float)val(c); float nx = (float)val((c & 0x80) | (((c & 0x7f) + 1) & 0x7f)); float mid = (v + nx) / 2; uint32_t b; memcpy(&b, &mid, 4); b += (int)(k % 5) - 2; f[i] = b; }
        else f[i] = (uint32_t)i * 1031u + (uint32_t)(i >> 7);
      }
      id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
      [c setComputePipelineState:p]; [c setBuffer:in offset:0 atIndex:0]; [c setBuffer:out offset:0 atIndex:1];
      [c dispatchThreads:MTLSizeMake(N,1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)]; [c endEncoding]; [cb commit]; [cb waitUntilCompleted];
      uint8_t *r = out.contents; int bad = 0, badh = 0;
      for (int i = 0; i < N; i++) {
        float x; memcpy(&x, &f[i], 4);
        for (int s = 0; s < 2; s++) { int w = ref(x, s); if (r[i*4+s] != w) { if (bad++ < 5) printf("  float %08x sat %d: got %#x want %#x\n", f[i], s, r[i*4+s], w); } }
        if (i < 65536) { __fp16 hh; uint16_t hb = i; memcpy(&hh, &hb, 2);
          for (int s = 0; s < 2; s++) { int w = ref((double)hh, s); if (r[i*4+2+s] != w) { if (badh++ < 5) printf("  half %04x sat %d: got %#x want %#x\n", hb, s, r[i*4+2+s], w); } } }
      }
      printf("%s math: %d float and %d half mismatches\n", fast ? "fast" : "safe", bad, badh);
      failed |= bad || badh;
    }
  }
  return failed;
}
