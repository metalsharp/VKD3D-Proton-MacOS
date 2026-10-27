#version 450
// The color from a uniform buffer in descriptor set 0.
layout(set = 0, binding = 0) uniform U { vec4 color; };
layout(location = 0) out vec4 o;
void main() { o = color; }
