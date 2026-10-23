#version 460
layout(location = 0) out uvec4 color;
void main() { color = uvec4(0x12345678u, 0xf0f0f0f0u, 0x0u, 0xffffffffu); }
