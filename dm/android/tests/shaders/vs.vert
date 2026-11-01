#version 450
// The translator's layout: the support buffer at binding 0 of the stage's set (vertex: 0); NVN attributes at their own locations
layout(set = 0, binding = 0) uniform Support { vec4 a; } support;
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 col;
layout(location = 0) out vec4 vcol;
void main() { gl_Position = pos; vcol = col; }
