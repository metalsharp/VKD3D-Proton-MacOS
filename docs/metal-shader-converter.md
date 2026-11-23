# Apple Metal Shader Converter

## What it is

`libmetalirconverter.dylib` is Apple's Metal Shader Converter library, © Apple Inc. It is proprietary, not open
source, and is licensed under Apple's *Software License Agreement for Metal Shader Converter*. The
`moltenvk-vX.Y-universal` package ships that agreement and Apple's acknowledgements file next to the library. The
library is the only part of the Metal Shader Converter that this repository distributes: not its command-line tool and
not its documentation.

## What it is used for

MoltenVK loads the library at runtime and calls its public C API, declared in the Apache-2.0 headers MoltenVK builds
with, to convert the DXIL shaders VKD3D-Proton hands it (and DXBC ones, through DirectXShaderCompiler's dxilconv) into
Metal libraries: compute shaders, and the vertex, mesh, amplification and pixel shaders of graphics pipelines. Without
it, every shader goes DXIL → SPIR-V → MSL text, and Metal compiles that text at run time: 0.4–1.7 s per large shader,
against 1–4 ms for the converter. Shader conversion is all the library does here. `METALSHARP_MSC=0` turns it off. If
the library is missing or a conversion fails, VKD3D-Proton builds that pipeline from SPIR-V as before.

## Why we may use and ship it this way

Our reading of Apple's agreement:

- **Section 2B** licenses the Dynamic Libraries (the converter's `.dylib` files) to be installed, used, tested and
  distributed "for the sole purpose of shader conversion". Converting D3D12 shaders for Metal is that purpose, and
  the library ships only for it.
- **Section 2E** forbids modifying, decompiling, disassembling or reverse engineering the software. The library
  ships byte for byte as Apple publishes it and keeps Apple's code signature: our scripts never re-sign it.
  `scripts/fetch-metal-shaderconverter.sh` checks its SHA-256 and Apple's signature. MoltenVK uses only the public
  API and the documented reflection the converter returns.
- **Sections 2C and 2D** limit the software to Apple-branded hardware and require copyright and proprietary notices
  to be kept. The library ships only in the macOS MoltenVK package, which runs only on Macs, with Apple's agreement
  and acknowledgements unmodified beside it. The library is not sold, rented or offered as a hosted service.
- **Section 7B** (and the agreement's opening note) asks that the software only be used with material you are
  permitted to use. It converts the shaders of the games you run, on your own Mac.
- Apple's own download requires an Apple ID, so this repository hosts the unmodified library, agreement and
  acknowledgements as the `deps-metal-shaderconverter-4.0b3` release, which section 2B's distribution grant allows
  ([release.md](release.md)).

Apple's agreement governs. If you redistribute the MoltenVK package, keep the library unmodified, with its agreement
and acknowledgements, and use it only for shader conversion on Apple-branded systems.
