// Texture buffer reads/writes at huge indices, and nil textures/buffers in an argument buffer.
#import <Metal/Metal.h>
#include <stdio.h>
static NSString* src = @"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct Args { texture2d<float> tex; device uint* buf; texture_buffer<uint, access::read_write> tb; };\n"
"kernel void tb_k(texture_buffer<uint, access::read_write> tb [[texture(0)]], device uint* out [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
"  uint idx = i == 0 ? 3u : i == 1 ? 16u : i == 2 ? 0x3ffffffcu : 0xffffffffu;\n"
"  out[i] = tb.read(idx).x;  if (i > 0) tb.write(uint4(0xdead), idx); }\n"
"kernel void nil_k(constant Args& a [[buffer(0)]], device float* out [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
"  constexpr sampler s; out[0] = a.tex.sample(s, float2(0.5)).x; out[1] = a.tex.read(uint2(0)).y; out[2] = as_type<float>(a.tb.read(0u).x); }\n";
int main(void) {
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError* err = nil;
    MTLCompileOptions* o = [MTLCompileOptions new]; o.languageVersion = MTLLanguageVersion3_1;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:o error:&err];
    if (!lib) { printf("compile: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    { // texture buffer of 16 uints
      id<MTLBuffer> backing = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
      uint* p = backing.contents; for (int k = 0; k < 16384; k++) p[k] = 0x5555; for (int k = 0; k < 16; k++) p[k] = 100 + k;
      MTLTextureDescriptor* d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:16 resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];
      id<MTLTexture> tb = [backing newTextureWithDescriptor:d offset:0 bytesPerRow:64];
      id<MTLBuffer> out = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
      id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
      [e setComputePipelineState:[dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"tb_k"] error:&err]];
      [e setTexture:tb atIndex:0]; [e setBuffer:out offset:0 atIndex:1];
      [e dispatchThreads:MTLSizeMake(4,1,1) threadsPerThreadgroup:MTLSizeMake(4,1,1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
      uint* r = out.contents;
      printf("texture buffer: status %ld reads [3]=%u [16]=%u [0x3ffffffc]=%u [~0]=%u; write past end landed at [16]: %s\n", (long)cb.status, r[0], r[1], r[2], r[3], p[16] == 0xdead ? "yes" : "no");
    }
    { // nil resources in an argument buffer
      id<MTLFunction> f = [lib newFunctionWithName:@"nil_k"];
      id<MTLArgumentEncoder> ae = [f newArgumentEncoderWithBufferIndex:0];
      id<MTLBuffer> args = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
      memset(args.contents, 0, ae.encodedLength);
      id<MTLBuffer> out = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
      float* r = out.contents; r[0] = r[1] = r[2] = 7.0f;
      id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
      [e setComputePipelineState:[dev newComputePipelineStateWithFunction:f error:&err]];
      [e setBuffer:args offset:0 atIndex:0]; [e setBuffer:out offset:0 atIndex:1];
      [e dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
      printf("nil in argument buffer: status %ld err %s sample=%g read=%g texbuf=%g\n", (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none", r[0], r[1], r[2]);
    }
  }
  return 0;
}
