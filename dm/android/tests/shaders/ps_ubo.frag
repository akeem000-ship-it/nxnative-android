#version 450
// cbuf slot 3 = the game's uniform buffer 0 of the pixel stage
layout(set = 1, binding = 0) uniform Support { vec4 a; } support;
layout(set = 1, binding = 1) uniform C3 { vec4 tint; } c3;
layout(location = 0) in vec4 vcol;
layout(location = 0) out vec4 o;
void main() { o = c3.tint; }
