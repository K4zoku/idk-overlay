#version 130

out vec2 v_texCoord;
void main()
{
    vec2 pos;
    vec2 tc;
    if (gl_VertexID == 0) { pos = vec2(-1.0, -1.0); tc = vec2(0.0, 0.0); }
    else if (gl_VertexID == 1) { pos = vec2( 1.0, -1.0); tc = vec2(1.0, 0.0); }
    else if (gl_VertexID == 2) { pos = vec2( 1.0,  1.0); tc = vec2(1.0, 1.0); }
    else if (gl_VertexID == 3) { pos = vec2(-1.0, -1.0); tc = vec2(0.0, 0.0); }
    else if (gl_VertexID == 4) { pos = vec2( 1.0,  1.0); tc = vec2(1.0, 1.0); }
    else { pos = vec2(-1.0,  1.0); tc = vec2(0.0, 1.0); }
    gl_Position = vec4(pos, 0.0, 1.0);
    v_texCoord = tc;
}
