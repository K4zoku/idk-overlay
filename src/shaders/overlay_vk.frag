#version 450

layout(binding = 0) uniform sampler2D overlay_tex;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag_color;

void main() {
    frag_color = texture(overlay_tex, uv);
}
