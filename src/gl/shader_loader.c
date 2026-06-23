#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl/gl_loader.h"
#include "gl/shader_loader.h"
#include "gl/shader.h"
#include "core/log.h"

/* ── Detect GL version from GL_VERSION string ──────────────────────── */

static void detect_gl_version(int *out_gl_version, bool *out_is_gles) {
    const char *version = (const char *)glGetString(GL_VERSION);
    int gl_version = 0;
    bool is_gles = false;

    if (version) {
        const char *es_prefixes[] = {
            "OpenGL ES-CM ",
            "OpenGL ES-CL ",
            "OpenGL ES ",
            NULL
        };
        for (int i = 0; es_prefixes[i]; i++) {
            size_t plen = strlen(es_prefixes[i]);
            if (strncmp(version, es_prefixes[i], plen) == 0) {
                version += plen;
                is_gles = true;
                break;
            }
        }
        int major = 0, minor = 0;
        sscanf(version, "%d.%d", &major, &minor);
        gl_version = major * 100 + minor * 10;
        if (is_gles && gl_version < 300)
            gl_version = 200;
        IDK_LOG("shdr", "GL version: %d.%d %s (g_gl_version=%d)\n",
                major, minor, is_gles ? "ES" : "", gl_version);
    }

    *out_gl_version = gl_version;
    *out_is_gles = is_gles;
}

/* ── Map GL version → GLSL version ─────────────────────────────────── */

static int glsl_version_for(int gl_version, bool is_gles) {
    if (!is_gles) {
        if (gl_version >= 410) return 410;
        if (gl_version >= 320) return 150;
        if (gl_version >= 300) return 130;
        return 120;
    } else {
        if (gl_version >= 300) return 300;
        return 100;
    }
}

/* ── Select shader variant by GLSL version ──────────────────────────── */

struct shader_variant {
    const char *ver_str;
    const char *vs_body;
    const char *fs_body;
    size_t vs_size;
    size_t fs_size;
    const unsigned char *vs_spirv;
    const unsigned char *fs_spirv;
    size_t vs_spirv_size;
    size_t fs_spirv_size;
};

static void select_variant(int glsl_version, bool is_gles,
                           struct shader_variant *v) {
    if (glsl_version <= 120) {
        v->ver_str = is_gles ? "#version 100\n" : "#version 120\n";
        v->vs_body = glsl_overlay_vertex_120;
        v->vs_size = GLSL_SHADER_SIZE(vertex_120);
        v->fs_body = glsl_overlay_fragment_120;
        v->fs_size = GLSL_SHADER_SIZE(fragment_120);
#ifdef HAS_SPV_120
        v->vs_spirv = spv_vertex_120;
        v->vs_spirv_size = SPV_SHADER_SIZE(vertex_120);
        v->fs_spirv = spv_fragment_120;
        v->fs_spirv_size = SPV_SHADER_SIZE(fragment_120);
#endif
    } else if (glsl_version == 300) {
        v->ver_str = "#version 300 es\n";
        v->vs_body = glsl_overlay_vertex_300_es;
        v->vs_size = GLSL_SHADER_SIZE(vertex_300_es);
        v->fs_body = glsl_overlay_fragment_300_es;
        v->fs_size = GLSL_SHADER_SIZE(fragment_300_es);
#ifdef HAS_SPV_300_es
        v->vs_spirv = spv_vertex_300_es;
        v->vs_spirv_size = SPV_SHADER_SIZE(vertex_300_es);
        v->fs_spirv = spv_fragment_300_es;
        v->fs_spirv_size = SPV_SHADER_SIZE(fragment_300_es);
#endif
    } else if (glsl_version >= 410) {
        v->ver_str = "#version 410 core\n";
        v->vs_body = glsl_overlay_vertex_410;
        v->vs_size = GLSL_SHADER_SIZE(vertex_410);
        v->fs_body = glsl_overlay_fragment_410;
        v->fs_size = GLSL_SHADER_SIZE(fragment_410);
#ifdef HAS_SPV_410
        v->vs_spirv = spv_vertex_410;
        v->vs_spirv_size = SPV_SHADER_SIZE(vertex_410);
        v->fs_spirv = spv_fragment_410;
        v->fs_spirv_size = SPV_SHADER_SIZE(fragment_410);
#endif
    } else {
        if (glsl_version >= 330)
            v->ver_str = "#version 330 core\n";
        else if (glsl_version >= 150)
            v->ver_str = "#version 150\n";
        else
            v->ver_str = "#version 130\n";
        v->vs_body = glsl_overlay_vertex_130;
        v->vs_size = GLSL_SHADER_SIZE(vertex_130);
        v->fs_body = glsl_overlay_fragment_130;
        v->fs_size = GLSL_SHADER_SIZE(fragment_130);
#ifdef HAS_SPV_130
        v->vs_spirv = spv_vertex_130;
        v->vs_spirv_size = SPV_SHADER_SIZE(vertex_130);
        v->fs_spirv = spv_fragment_130;
        v->fs_spirv_size = SPV_SHADER_SIZE(fragment_130);
#endif
    }
}

/* ── Try SPIR-V compile ─────────────────────────────────────────────
 * Returns 1 if both vertex and fragment compile successfully.
 * Leaves *out_vs / *out_fs zeroed on failure.                         */

static int try_spirv(unsigned int *out_vs, unsigned int *out_fs,
                     const unsigned char *vs_spv, size_t vs_spv_size,
                     const unsigned char *fs_spv, size_t fs_spv_size) {
    int ok;

    *out_vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderBinary(1, out_vs, GL_SHADER_BINARY_FORMAT_SPIR_V,
                   vs_spv, (GLsizei)vs_spv_size);
    glSpecializeShader(*out_vs, "main", 0, NULL, NULL);
    glCompileShader(*out_vs);
    glGetShaderiv(*out_vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        IDK_LOG("shdr", "SPIR-V vs failed\n");
        glDeleteShader(*out_vs);
        *out_vs = 0;
        return 0;
    }

    *out_fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderBinary(1, out_fs, GL_SHADER_BINARY_FORMAT_SPIR_V,
                   fs_spv, (GLsizei)fs_spv_size);
    glSpecializeShader(*out_fs, "main", 0, NULL, NULL);
    glCompileShader(*out_fs);
    glGetShaderiv(*out_fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        IDK_LOG("shdr", "SPIR-V fs failed\n");
        glDeleteShader(*out_vs);
        glDeleteShader(*out_fs);
        *out_vs = 0;
        *out_fs = 0;
        return 0;
    }

    return 1;
}

/* ── Try GLSL compile ────────────────────────────────────────────────
 * Returns 1 if both vertex and fragment compile successfully.          */

static int try_glsl(unsigned int *out_vs, unsigned int *out_fs,
                    const char *ver_str,
                    const char *vs_body, size_t vs_size,
                    const char *fs_body, size_t fs_size) {
    int ok;

    if (!idk_fn_glCreateShader || !idk_fn_glShaderSource || !idk_fn_glCompileShader ||
        !idk_fn_glGetShaderiv) {
        IDK_ERR("shdr", "try_glsl: critical GL shader functions are NULL — cannot compile\n");
        return 0;
    }

    const GLchar *vs_src[] = { ver_str, vs_body };
    const GLint  vs_len[]  = { (GLint)strlen(ver_str), (GLint)vs_size };
    *out_vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(*out_vs, 2, vs_src, vs_len);
    glCompileShader(*out_vs);
    glGetShaderiv(*out_vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar log[512];
        glGetShaderiv(*out_vs, GL_INFO_LOG_LENGTH, &ok);
        if (ok > 0) {
            glGetShaderInfoLog(*out_vs, 512, NULL, log);
            IDK_ERR("shdr", "VS log:\n%s\n", log);
        }
        glDeleteShader(*out_vs);
        *out_vs = 0;
        return 0;
    }

    const GLchar *fs_src[] = { ver_str, fs_body };
    const GLint  fs_len[]  = { (GLint)strlen(ver_str), (GLint)fs_size };
    *out_fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(*out_fs, 2, fs_src, fs_len);
    glCompileShader(*out_fs);
    glGetShaderiv(*out_fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar log[512];
        glGetShaderiv(*out_fs, GL_INFO_LOG_LENGTH, &ok);
        if (ok > 0) {
            glGetShaderInfoLog(*out_fs, 512, NULL, log);
            IDK_ERR("shdr", "FS log:\n%s\n", log);
        }
        glDeleteShader(*out_vs);
        glDeleteShader(*out_fs);
        *out_vs = 0;
        *out_fs = 0;
        return 0;
    }

    return 1;
}

/* ── Link program ───────────────────────────────────────────────────
 * Takes ownership of vs/fs (deletes them). Returns 0 on failure.       */

static unsigned int link_program(unsigned int vs, unsigned int fs) {
    int ok;

    unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar log[512];
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &ok);
        if (ok > 0) {
            glGetProgramInfoLog(prog, 512, NULL, log);
            IDK_ERR("shdr", "Link log: %s\n", log);
        }
        glDeleteProgram(prog);
        prog = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

/* ── Check SPIR-V driver support ────────────────────────────────────── */

int idk_shader_loader_has_spirv(void) {
    if (idk_fn_glShaderBinary) {
        GLint formats[16] = {0};
        glGetIntegerv(GL_SHADER_BINARY_FORMATS, formats);
        for (int i = 0; i < 16 && formats[i]; i++) {
            if (formats[i] == GL_SHADER_BINARY_FORMAT_SPIR_V)
                return 1;
        }
    }
    return 0;
}

/* ── Compile program (SPIR-V preferred, GLSL fallback) ─────────────────
 * Returns linked program handle, or 0 on failure.                       */

static unsigned int compile_program(struct shader_variant *v) {
    unsigned int vs = 0, fs = 0;

    if (v->vs_spirv && v->vs_spirv_size > 4 &&
        v->fs_spirv && v->fs_spirv_size > 4 &&
        idk_shader_loader_has_spirv()) {

        if (try_spirv(&vs, &fs,
                      v->vs_spirv, v->vs_spirv_size,
                      v->fs_spirv, v->fs_spirv_size)) {
            IDK_LOG("shdr", "SPIR-V compilation OK\n");
            return link_program(vs, fs);
        }
        IDK_LOG("shdr", "SPIR-V failed, fallback to GLSL\n");
    }

    IDK_LOG("shdr", "Using %s shader variant\n", v->ver_str);

    if (!try_glsl(&vs, &fs,
                  v->ver_str,
                  v->vs_body, v->vs_size,
                  v->fs_body, v->fs_size))
        return 0;

    return link_program(vs, fs);
}

/* ── Public API ─────────────────────────────────────────────────────── */

unsigned int idk_shader_loader_init(int *out_gl_version, bool *out_is_gles) {
    int gl_version;
    bool is_gles;

    detect_gl_version(&gl_version, &is_gles);

    if (out_gl_version) *out_gl_version = gl_version;
    if (out_is_gles)    *out_is_gles    = is_gles;

    int glsl_ver = glsl_version_for(gl_version, is_gles);
    struct shader_variant v = {0};
    select_variant(glsl_ver, is_gles, &v);

    return compile_program(&v);
}
