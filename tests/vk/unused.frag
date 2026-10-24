#version 450
/* Red to both color outputs. */
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
void main()
{
    o0 = vec4(1.0, 0.0, 0.0, 1.0);
    o1 = vec4(1.0, 0.0, 0.0, 1.0);
}
