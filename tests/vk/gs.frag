#version 450
layout(location = 0) flat in uvec4 color;
layout(location = 0) out uvec4 o;
void main()
{
    o = color;
}
