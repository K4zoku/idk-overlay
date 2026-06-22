#version 410 core

uniform sampler2D u_texture;
in vec2 v_texCoord;
layout(location = 0) out vec4 outColor;
void main()
{
    outColor = texture(u_texture, v_texCoord);
}
