#!/usr/bin/env bash
# Compile every checked-in HLSL probe shader with the pinned DXC tool.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DXC="${DXC:-dxc}"
OUT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/vkd3d-hlsl-dxil"

command -v "$DXC" >/dev/null 2>&1 || {
    echo "dxc is required; set DXC to the DirectX Shader Compiler binary" >&2
    exit 1
}
rm -rf "$OUT"
mkdir -p "$OUT"

compile() {
    local source="$1" entry="$2" profile="$3" output="$4"
    local -a flags=(-nologo -HV 2021)
    # The InnerCoverage probe intentionally targets the custom rasterization
    # path; current DXC rejects its DXIL intrinsic overload during validation
    # even though it emits the shader used by the runtime probe.
    if [ "$source" = scripts/probes/cr-inner/inner.hlsl ]; then
        flags+=(-Vd)
    fi
    echo "dxc $profile $source::$entry"
    "$DXC" "${flags[@]}" -E "$entry" -T "$profile" \
        -Fo "$OUT/$output" "$ROOT/$source"
}

while IFS= read -r source; do
    case "$source" in
        scripts/probes/core10/corpus/mesh-triangle.hlsl)
            compile "$source" main ms_6_5 "$(basename "$source").dxil" ;;
        scripts/probes/cr-inner/inner.hlsl)
            compile "$source" main ps_6_0 "$(basename "$source").dxil" ;;
        scripts/probes/cr-inner/inner_vs.hlsl)
            compile "$source" main vs_6_0 "$(basename "$source").dxil" ;;
        scripts/probes/dxgi-present/triangle.hlsl)
            compile "$source" vs vs_6_0 triangle-vs.dxil
            compile "$source" ps ps_6_0 triangle-ps.dxil ;;
        scripts/probes/feedback/feedback.hlsl)
            compile "$source" main ps_6_5 "$(basename "$source").dxil" ;;
        scripts/probes/core10/corpus/*.hlsl)
            compile "$source" main cs_6_0 "$(basename "$source").dxil" ;;
        # Metal Shader Converter bridge tests, with tests/msc-bridge/build.sh's profiles
        tests/msc-bridge/hlsl/cs3.hlsl)
            compile "$source" main cs_6_6 "msc-$(basename "$source").dxil" ;;
        tests/msc-bridge/hlsl/*cs*.hlsl)
            compile "$source" main cs_6_0 "msc-$(basename "$source").dxil" ;;
        tests/msc-bridge/hlsl/*vs.hlsl)
            compile "$source" main vs_6_0 "msc-$(basename "$source").dxil" ;;
        tests/msc-bridge/hlsl/*ps.hlsl)
            compile "$source" main ps_6_0 "msc-$(basename "$source").dxil" ;;
        *)
            echo "No DXC profile mapping for $source" >&2
            exit 1 ;;
    esac
done < <(cd "$ROOT" && git ls-files '*.hlsl' | sort)

count=$(find "$OUT" -type f -name '*.dxil' | wc -l | tr -d ' ')
[ "$count" -eq 23 ] || { echo "expected 23 DXIL outputs, found $count" >&2; exit 1; }
echo "HLSL/DXIL source check: PASS ($count shaders)"
