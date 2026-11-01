#version 450
// a texture from combined handle slot 0 (c2 word 8 = byte 0x20)
layout(set = 1, binding = 0) uniform Support { vec4 a; } support;
layout(set = 1, binding = 2) uniform sampler2D tex;
layout(location = 0) in vec4 vcol;
layout(location = 0) out vec4 o;
void main() { o = texture(tex, vcol.xy); }
