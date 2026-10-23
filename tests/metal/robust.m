// Out-of-bounds buffer reads and writes on Apple GPUs: direct bindings vs argument-buffer GPU addresses.
#import <Metal/Metal.h>
#include <stdio.h>
static NSString* src = @"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct Args { device uint* data; };\n"
"kernel void direct_k(device uint* data [[buffer(0)]], device uint* out [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
"  out[i] = data[i * 64u];  data[4096u + i] = 0xdeadu; }\n"
"kernel void argbuf_k(constant Args& a [[buffer(0)]], device uint* out [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
"  out[i] = a.data[i * 64u];  a.data[4096u + i] = 0xdeadu; }\n";
int main(void) {
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSError* err = nil;
    MTLCompileOptions* opts = [MTLCompileOptions new]; opts.languageVersion = MTLLanguageVersion3_1;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opts error:&err];
    if (!lib) { printf("compile: %s\n", err.localizedDescription.UTF8String); return 1; }
    // 16 uints of data inside a 64 KiB allocation of 0x5555 guard words, so OOB reads see real memory
    id<MTLBuffer> big = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    uint* p = big.contents; for (int k = 0; k < 16384; k++) p[k] = 0x5555;
    for (int k = 0; k < 16; k++) p[k] = 100 + k;
    for (int mode = 0; mode < 3; mode++) {
      id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:mode == 1 ? @"argbuf_k" : @"direct_k"] error:&err];
      id<MTLBuffer> out = [dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
      id<MTLCommandQueue> q = [dev newCommandQueue];
      id<MTLCommandBuffer> cb = [q commandBuffer];
      id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
      [e setComputePipelineState:ps];
      id<MTLBuffer> small = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
      uint* sp = small.contents; for (int k = 0; k < 16; k++) sp[k] = 100 + k;
      if (mode == 2) {
        [e setBuffer:big offset:4096 atIndex:0];  /* a 16-uint "descriptor" inside a 64 KiB heap */
        for (int k = 0; k < 16; k++) p[1024 + k] = 100 + k;
      } else if (mode == 0) {
        [e setBuffer:small offset:0 atIndex:0];
      } else {
        id<MTLBuffer> args = [dev newBufferWithLength:8 options:MTLResourceStorageModeShared];
        *(uint64_t*)args.contents = small.gpuAddress;
        [e setBuffer:args offset:0 atIndex:0];
        [e useResource:small usage:MTLResourceUsageRead | MTLResourceUsageWrite];
      }
      [e setBuffer:out offset:0 atIndex:1];
      [e dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
      [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
      uint* o = out.contents;
      uint* sp2 = mode == 2 ? p + 1024 : (uint*)small.contents;
      printf("  writes past the end landed: %s\n", (mode == 2 ? p[1024 + 4096] : 0) == 0xdead ? "yes (in the heap)" : "no / outside");
      (void)sp2;
      printf("%s: status %ld err %s reads [0]=%u [64]=%u [128]=%u [192]=%u\n", mode == 1 ? "argument-buffer address" : mode == 2 ? "offset in a heap" : "direct binding",
             (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none", o[0], o[1], o[2], o[3]);
    }
  }
  return 0;
}
