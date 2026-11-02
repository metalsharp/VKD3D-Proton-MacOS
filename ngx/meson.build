# VKD3D-Proton-MacOS's NGX core (nvngx.dll) and NVAPI (nvapi64.dll), for this lane's DLSS on MetalFX: built ARM64X
# like VKD3D-Proton (scripts/build-ngx-arm64x.sh). NVIDIA's libraries load them by these names.
project('vkd3d-proton-macos-ngx', ['cpp'], version : '1.0.0', meson_version : '>= 0.60',
  default_options : ['cpp_std=c++20', 'warning_level=1', 'buildtype=release', 'b_ndebug=if-release'])

cpp = meson.get_compiler('cpp')
link_args = ['-static', '-Wl,--enable-stdcall-fixup']

# ID3D12GraphicsCommandListMetalFX, VKD3D-Proton's (sources/vkd3d-proton)
vkd3d_include = include_directories('../sources/vkd3d-proton/include/private')
# NVIDIA's NVAPI headers (sources/nvapi, NVIDIA/nvapi)
nvapi_include = include_directories('../sources/nvapi')

shared_library('nvngx', 'nvngx/nvngx.cpp',
  name_prefix         : '',
  include_directories : [vkd3d_include],
  link_args           : link_args)

shared_library('nvapi64', 'nvapi/nvapi.cpp',
  name_prefix         : '',
  include_directories : [nvapi_include],
  dependencies        : [cpp.find_library('dxgi')],
  vs_module_defs      : 'nvapi/nvapi64.def',
  link_args           : link_args)
