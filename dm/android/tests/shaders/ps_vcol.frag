#version 450
layout(set = 1, binding = 0) uniform Support { vec4 a; } support;
layout(location = 0) in vec4 vcol;
layout(location = 0) out vec4 o;
void main() { o = vcol; }
