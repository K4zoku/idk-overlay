#version 450

/* Fullscreen triangle (covers clip space [-1,-1]..[1,1] with one oversized tri).
 * NOTE: Vulkan's framebuffer origin is TOP-left (clip y=-1 → top of screen),
 * unlike OpenGL where clip y=-1 is bottom of screen. Combined with:
 *   - SHM data layout: byte 0 = bottom row of Qt content (glReadPixels is
 *     bottom-up in OpenGL convention).
 *   - vkCmdCopyBufferToImage: byte 0 → texel (0,0) → sampled at uv=(0,0)
 *   - Vulkan sampling: uv=(0,0) = top-left of image
 * …the VkImage ends up vertically flipped relative to the original Qt content.
 * To display correctly, we need to flip V so that:
 *   - clip y=-1 (TOP of screen) samples uv.y=1 (bottom row of VkImage =
 *     top row of Qt content)
 *   - clip y=+1 (BOTTOM of screen) samples uv.y=0 (top row of VkImage =
 *     bottom row of Qt content)
 * Hence: uv.y = 0.5 - clip.y * 0.5  (i.e. 1.0 - (clip.y * 0.5 + 0.5)). */
const vec2 verts[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

layout(location = 0) out vec2 uv;

void main() {
    vec2 p = verts[gl_VertexIndex];
    uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    gl_Position = vec4(p, 0.0, 1.0);
}

