#version 450
#extension GL_NV_conservative_raster_underestimation : require
layout(location = 0) in vec2 v;
layout(location = 0) out vec4 o;
void main()
{
    o = vec4(1.0, gl_FragFullyCoveredNV ? 1.0 : 0.0, fract(v.x), 1.0);
}
