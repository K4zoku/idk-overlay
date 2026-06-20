/*
 * overlay_shader.h — Minimal GL shader for compositing overlay frames
 *
 * Renders a fullscreen quad with the overlay texture on top of the
 * game's framebuffer. Uses direct vertex coordinates (no VBO/VAO).
 */

#ifndef IDK_OVERLAY_SHADER_H
#define IDK_OVERLAY_SHADER_H

/* ── Vertex shader: fullscreen quad ─────────────────────────────────── */
static const char *overlay_vertex_shader =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec2 a_texCoord;\n"
    "out vec2 v_texCoord;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texCoord = a_texCoord;\n"
    "}\n";

/* ── Fragment shader: sample overlay texture, blend with alpha ──────── */
static const char *overlay_fragment_shader =
    "#version 330 core\n"
    "in vec2 v_texCoord;\n"
    "out vec4 outColor;\n"
    "uniform sampler2D u_texture;\n"
    "void main()\n"
    "{\n"
    "    vec4 overlay = texture(u_texture, v_texCoord);\n"
    "    outColor = overlay;\n"
    "}\n";

#endif /* IDK_OVERLAY_SHADER_H */
