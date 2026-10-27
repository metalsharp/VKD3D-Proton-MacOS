/* Offline probe of the Metal Shader Converter bridge: converts dumped DXIL with a dumped D3D12 root signature
 * (the RTS0 blob VKD3D-Proton writes as <hash>.rs under METALSHARP_TRACE=shader) the way MoltenVK does
 * (MVKShaderModule::convertMSC), and prints the converter's reflection and the root signature's argument buffer
 * layout next to the layout VKD3D-Proton pushes (d3d12_root_signature_init_msc). Nothing runs on the GPU.
 *   mscprobe <root signature .rs> <shader .dxil>...   (libmetalirconverter.dylib from DYLD_LIBRARY_PATH) */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <metal_irconverter.h>

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    void *data;
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(*size);
    if (fread(data, 1, *size, f) != *size) { perror(path); exit(1); }
    fclose(f);
    return data;
}

static const char *type_names[] = { "table", "constants", "CBV", "SRV", "UAV" };
static const char *vis_names[] = { "all", "vertex", "hull", "domain", "geometry", "pixel", "amplification", "mesh" };

int main(int argc, char **argv)
{
    uint32_t version, param_count, param_offset, sampler_count, sampler_offset, flags, i, j;
    IRVersionedRootSignatureDescriptor rs_desc = {0};
    IRRootParameter1 *params;
    IRStaticSamplerDescriptor *samplers;
    IRRootSignature *root_sig;
    IRError *err = NULL;
    uint64_t offset = 0;
    const uint32_t *w;
    size_t rs_size;
    uint8_t *rs;
    int a;

    if (argc < 3) { fprintf(stderr, "usage: %s <root signature .rs> <shader .dxil>...\n", argv[0]); return 1; }

    rs = read_file(argv[1], &rs_size);
    w = (const uint32_t *)rs;
    version = w[0]; param_count = w[1]; param_offset = w[2]; sampler_count = w[3]; sampler_offset = w[4]; flags = w[5];
    if (version != 2) { fprintf(stderr, "Root signature version %#x: only 1.1 is handled.\n", version); return 1; }
    printf("Root signature %s: %u parameters, %u static samplers, flags %#x\n", argv[1], param_count, sampler_count, flags);

    params = calloc(param_count, sizeof(*params));
    for (i = 0; i < param_count; i++)
    {
        const uint32_t *p = (const uint32_t *)(rs + param_offset + i * 12);
        const uint32_t *d = (const uint32_t *)(rs + p[2]);
        params[i].ParameterType = p[0];
        params[i].ShaderVisibility = p[1];
        /* VKD3D-Proton's top-level layout: constants 4-byte aligned, root descriptors and tables 8 bytes. */
        if (p[0] == IRRootParameterType32BitConstants)
        {
            offset = (offset + 3) & ~3ull;
            printf("  [%u] %-9s vis %-6s b%u space %u, %u words: vkd3d offset %llu\n", i, type_names[p[0]],
                    vis_names[p[1]], d[0], d[1], d[2], (unsigned long long)offset);
            params[i].Constants = (IRRootConstants){ d[0], d[1], d[2] };
            offset += d[2] * 4;
        }
        else if (p[0] == IRRootParameterTypeDescriptorTable)
        {
            const uint32_t *r = (const uint32_t *)(rs + d[1]);
            IRDescriptorRange1 *ranges = calloc(d[0], sizeof(*ranges));
            offset = (offset + 7) & ~7ull;
            printf("  [%u] %-9s vis %-6s %u ranges: vkd3d offset %llu\n", i, type_names[p[0]], vis_names[p[1]], d[0],
                    (unsigned long long)offset);
            for (j = 0; j < d[0]; j++, r += 6)
            {
                ranges[j] = (IRDescriptorRange1){ r[0], r[1], r[2], r[3], r[4], r[5] };
                printf("        range type %u, %u descriptors from %u space %u, flags %#x, table offset %u\n",
                        r[0], r[1], r[2], r[3], r[4], r[5]);
            }
            params[i].DescriptorTable = (IRRootDescriptorTable1){ d[0], ranges };
            offset += 8;
        }
        else
        {
            offset = (offset + 7) & ~7ull;
            printf("  [%u] %-9s vis %-6s reg %u space %u flags %#x: vkd3d offset %llu\n", i, type_names[p[0]],
                    vis_names[p[1]], d[0], d[1], d[2], (unsigned long long)offset);
            params[i].Descriptor = (IRRootDescriptor1){ d[0], d[1], d[2] };
            offset += 8;
        }
    }
    offset = (offset + 7) & ~7ull;
    printf("  vkd3d top-level size %llu, heaps at %s%llu\n", (unsigned long long)offset,
            flags & 0xc00 ? "" : "(none) ", (unsigned long long)offset);

    samplers = calloc(sampler_count ? sampler_count : 1, sizeof(*samplers));
    for (i = 0; i < sampler_count; i++)
        memcpy(&samplers[i], rs + sampler_offset + i * 52, 52);

    rs_desc.version = IRRootSignatureVersion_1_1;
    rs_desc.desc_1_1.NumParameters = param_count;
    rs_desc.desc_1_1.pParameters = params;
    rs_desc.desc_1_1.NumStaticSamplers = sampler_count;
    rs_desc.desc_1_1.pStaticSamplers = samplers;
    rs_desc.desc_1_1.Flags = flags;
    if (!(root_sig = IRRootSignatureCreateFromDescriptor(&rs_desc, &err)))
    {
        fprintf(stderr, "Creating the root signature failed: %s\n", err ? (const char *)IRErrorGetPayload(err) : "?");
        return 1;
    }
    {
        size_t n = IRRootSignatureGetResourceCount(root_sig);
        IRResourceLocation *locs = calloc(n ? n : 1, sizeof(*locs));
        IRRootSignatureGetResourceLocations(root_sig, locs);
        printf("Converter root signature layout (%zu entries):\n", n);
        for (i = 0; i < n; i++)
            printf("  type %u space %u slot %u: offset %u, %llu bytes\n", locs[i].resourceType, locs[i].space,
                    locs[i].slot, locs[i].topLevelOffset, (unsigned long long)locs[i].sizeBytes);
    }

    for (a = 2; a < argc; a++)
    {
        static const IRShaderStage stages[] = { IRShaderStageVertex, IRShaderStageFragment, IRShaderStageCompute };
        IRCompiler *compiler = IRCompilerCreate();
        IRShaderReflection *refl;
        IRObject *dxil, *ir;
        const char *json;
        size_t size, n;
        uint8_t *code;
        IRShaderStage stage = IRShaderStageInvalid;

        code = read_file(argv[a], &size);
        IRCompilerIgnoreRootSignature(compiler, true);
        IRCompilerSetGlobalRootSignature(compiler, root_sig);
        IRCompilerSetCompatibilityFlags(compiler, IRCompatibilityFlagBoundsCheck | IRCompatibilityFlagDisableNanInfOptimization);
        dxil = IRObjectCreateFromDXIL(code, size, IRBytecodeOwnershipNone);
        err = NULL;
        if (!(ir = IRCompilerAllocCompileAndLink(compiler, NULL, dxil, &err)))
        {
            printf("\n== %s: conversion failed: %s\n", argv[a], err ? (const char *)IRErrorGetPayload(err) : "?");
            continue;
        }
        for (i = 0; i < 3; i++)
            if (IRObjectGetMetalIRShaderStage(ir) == stages[i])
                stage = stages[i];
        refl = IRShaderReflectionCreate();
        IRObjectGetReflection(ir, stage, refl);
        printf("\n== %s: stage %d, entry point %s\n", argv[a], stage, IRShaderReflectionGetEntryPointFunctionName(refl));
        n = IRShaderReflectionGetResourceCount(refl);
        {
            IRResourceLocation *locs = calloc(n ? n : 1, sizeof(*locs));
            IRShaderReflectionGetResourceLocations(refl, locs);
            printf("Resources the shader reads from the top-level argument buffer (%zu):\n", n);
            for (i = 0; i < n; i++)
                printf("  type %u space %u slot %u: offset %u, %llu bytes (%s)\n", locs[i].resourceType, locs[i].space,
                        locs[i].slot, locs[i].topLevelOffset, (unsigned long long)locs[i].sizeBytes,
                        locs[i].resourceName ? locs[i].resourceName : "");
        }
        if (stage == IRShaderStageVertex)
        {
            IRVersionedVSInfo vs = { .version = IRReflectionVersion_1_0 };
            IRShaderReflectionCopyVertexInfo(refl, IRReflectionVersion_1_0, &vs);
            printf("Vertex inputs %zu, needs draw params %d, output size %zu\n", (size_t)vs.info_1_0.num_vertex_inputs,
                    vs.info_1_0.needs_draw_params, (size_t)vs.info_1_0.vertex_output_size_in_bytes);
            IRShaderReflectionReleaseVertexInfo(&vs);
        }
        json = IRShaderReflectionCopyJSONString(refl);
        printf("Reflection JSON:\n%s\n", json);
        IRShaderReflectionReleaseString(json);
        IRShaderReflectionDestroy(refl);
        IRObjectDestroy(ir);
        IRObjectDestroy(dxil);
        IRCompilerDestroy(compiler);
        free(code);
    }
    IRRootSignatureDestroy(root_sig);
    return 0;
}
