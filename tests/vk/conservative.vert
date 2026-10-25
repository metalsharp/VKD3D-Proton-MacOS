#version 450
/* Positions come in scaled by 2 and the uniform/push constants undo it, so the object stage's
 * descriptor and push constant bindings are exercised. */
layout(location = 0) in vec2 pos;
layout(location = 0) out vec2 v;
layout(set = 0, binding = 0) uniform U { vec4 xform; };
layout(push_constant) uniform P { vec2 bias; };
void main()
{
    gl_Position = vec4(pos * xform.xy + xform.zw + bias, 0.5, 1.0);
    v = pos;
}
