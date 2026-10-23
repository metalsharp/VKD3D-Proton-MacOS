#version 450
layout(location = 0) in vec2 v;
layout(location = 0) out vec4 o;
void main()
{
    o = vec4(1.0, fract(v), 1.0);
}
