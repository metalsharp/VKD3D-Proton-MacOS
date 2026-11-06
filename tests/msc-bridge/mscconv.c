/* Converts DXIL with the Metal Shader Converter and writes the metallib, to read its AIR (metal-objdump -d) when
 * comparing what different DXIL becomes, and prints how many top-level resources its reflection lists. No root signature: the converter's default layout. Compatibility flags as
 * MoltenVK sets them with robust buffer access (bounds checked, NaN and Inf kept).
 *   mscconv <in.dxil> <out.metallib>   (libmetalirconverter.dylib from DYLD_LIBRARY_PATH) */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <metal_irconverter.h>

int main(int argc, char **argv)
{
    IRError *err = NULL;
    IRCompiler *compiler;
    IRObject *dxil, *ir;
    IRMetalLibBinary *lib;
    size_t size;
    uint8_t *data, *bytes;
    FILE *f;

    if (argc != 3) { fprintf(stderr, "usage: mscconv <in.dxil> <out.metallib>\n"); return 2; }
    if (!(f = fopen(argv[1], "rb"))) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    data = malloc(size);
    if (fread(data, 1, size, f) != size) { perror(argv[1]); return 1; }
    fclose(f);

    compiler = IRCompilerCreate();
    IRCompilerSetCompatibilityFlags(compiler, IRCompatibilityFlagBoundsCheck | IRCompatibilityFlagDisableNanInfOptimization);
    dxil = IRObjectCreateFromDXIL(data, size, IRBytecodeOwnershipNone);
    if (!(ir = IRCompilerAllocCompileAndLink(compiler, NULL, dxil, &err)))
    {
        fprintf(stderr, "conversion failed: %s\n", err ? (const char *)IRErrorGetPayload(err) : "?");
        return 1;
    }
    {
        IRShaderReflection *reflection = IRShaderReflectionCreate();
        IRObjectGetReflection(ir, IRObjectGetMetalIRShaderStage(ir), reflection);
        printf("%s: %zu top-level resources\n", argv[1], IRShaderReflectionGetResourceCount(reflection));
        IRShaderReflectionDestroy(reflection);
    }
    lib = IRMetalLibBinaryCreate();
    IRObjectGetMetalLibBinary(ir, IRObjectGetMetalIRShaderStage(ir), lib);
    size = IRMetalLibGetBytecodeSize(lib);
    bytes = malloc(size);
    IRMetalLibGetBytecode(lib, bytes);
    {
        /* Not world-writable: 0644 at most. */
        int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || write(fd, bytes, size) != (ssize_t)size) { perror(argv[2]); return 1; }
        close(fd);
    }
    return 0;
}
