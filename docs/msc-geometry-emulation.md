# Metal Shader Converter geometry and tessellation emulation: what the converted code expects

Not used yet: geometry shaders and tessellation still go through SPIR-V and MoltenVK's own mesh-shader emulation.
These are notes for a bridge path, read from the converter's output (`IRCompilerEnableGeometryAndTessellationEmulation`,
then `metal-objdump -d` on the metallib, which prints the AIR), since Apple's runtime header with the draw helpers is
not available here.

## Pipeline

- The vertex shader, converted with emulation on, has an object function `<entry>.dxil_irconverter_object_shader` next
  to the vertex function. The geometry shader converts to a mesh function. A hull shader is called from the object
  function (`irconverter_hull_shader`), a domain shader from the mesh function.
- Reflection: `IRShaderReflectionCopyGeometryInfo` gives `max_input_primitives_per_mesh_threadgroup` (64 for a
  triangle-list GS), `max_payload_size_in_bytes` (16256), `instance_count`, `input_primitive`, `is_passthrough`;
  `IRShaderReflectionCopyVertexInfo` gives `vertex_output_size_in_bytes`.
- Function constants: `tessellationEnabled` (uint8), `vertex_shader_output_size_fc` (uint32).

## Object function bindings (buffer indexes)

| Index | Argument |
|---:|---|
| 0, 1 | resource and sampler heaps |
| 2 | top-level argument buffer (vertex stage) |
| 3 | top-level argument buffer (hull stage) |
| 4 | `DrawArguments`: 5 x uint32, `IRRuntimeDrawArgument` or `IRRuntimeDrawIndexedArgument` |
| 5 | uniforms: `IRRuntimeDrawInfo` (24 bytes), see below |
| 6 | vertex buffers: `IRRuntimeVertexBuffers`, 31 x { uint64 address, uint32 length, uint32 stride } |
| 7 | tessellator lookup tables (`kIRRuntimeTessellatorTablesBindPoint`) |

`IRRuntimeDrawInfo`, as the code reads it: uint16 index type (0: not indexed, else MTLIndexType + 1), uint8 primitive
topology (`IRRuntimePrimitiveType`), uint8 threads per patch, uint16 max input primitives per mesh threadgroup, uint16
object threadgroup vertex stride, uint16 mesh threadgroup primitive stride (read as the primitives per mesh
threadgroup), uint16 GS instance count, uint16 patches per object threadgroup, uint16 input control points per patch,
uint64 index buffer address.

## Dispatch, without tessellation (from the object function's code)

- Each object threadgroup takes `object_threadgroup_vertex_stride` vertices, one thread each; its threadgroup index x
  is the slice, y the instance (instance ID in the payload).
- Vertices per primitive come from the topology: 1, 2 or 3, doubled with adjacency. List topologies:
  `stride / verticesPerPrim` primitives per object threadgroup, `vertexCount / verticesPerPrim` in all. Strips:
  `stride` primitives per threadgroup, `vertexCount - (verticesPerPrim - 1)` in all.
- Thread 0 writes the payload header (primitive count, first primitive, instance ID) and sets the mesh grid to
  (min(ceil(primitives / primitives per mesh threadgroup), 1024), GS instance count, 1).
- The payload holds every vertex's outputs after a 20-byte header: the stride times `vertex_output_size_in_bytes`
  must fit in `max_payload_size_in_bytes - 20`.

Open: the vertex fetch (the converted object function reads `vertex_buffers_ab`; the stage-in may need
`IRMetalLibSynthesizeStageInFunction` with the pipeline's input layout), the mesh threadgroup's thread count, strips'
overlap between object threadgroups, and the tessellation path (hull and domain).
