// vr_foveated.cpp -- see vr_foveated.hpp.

#include "vr_foveated.hpp"
#include "vr_cvars.hpp"
#include "vr_gfx.hpp"
#include "vr_profile.hpp"
#include "vr_upscale.hpp"

#include "Zancle/Base/GetArraySize.hpp"
#include "Zancle/Base/SizeT.hpp"
#include "Zancle/Base/Strcmp.hpp"
#include "Zancle/Container/Vector.hpp"
#include "Zancle/Math/Clamp.hpp"
#include "Zancle/Math/MinMax.hpp"
#include "Zancle/Math/Tan.hpp"
#include "Zancle/String/String.hpp"


#ifndef GL_SHADING_RATE_IMAGE_NV
#define GL_SHADING_RATE_IMAGE_NV 0x9563
#endif
#ifndef GL_SHADING_RATE_1_INVOCATION_PER_PIXEL_NV
#define GL_SHADING_RATE_1_INVOCATION_PER_PIXEL_NV 0x9565
#define GL_SHADING_RATE_1_INVOCATION_PER_2X2_PIXELS_NV 0x9568
#define GL_SHADING_RATE_1_INVOCATION_PER_4X4_PIXELS_NV 0x956B
#endif
#ifndef GL_SHADING_RATE_IMAGE_TEXEL_WIDTH_NV
#define GL_SHADING_RATE_IMAGE_TEXEL_WIDTH_NV 0x955C
#define GL_SHADING_RATE_IMAGE_TEXEL_HEIGHT_NV 0x955D
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER 0x8D94
#endif

namespace qvr::foveated
{
namespace
{

using BindShadingRateImageFn = void(APIENTRY*)(GLuint texture);
using ShadingRateImagePaletteFn = void(APIENTRY*)(GLuint viewport, GLuint first, GLsizei count, const GLenum* rates);
BindShadingRateImageFn bindShadingRateImage = nullptr;
ShadingRateImagePaletteFn shadingRateImagePalette = nullptr;

int support = -1; // unknown
int tileWidth = 16;
int tileHeight = 16;

// The palette: a texel's value is an index into it.
constexpr GLenum palette[] = {GL_SHADING_RATE_1_INVOCATION_PER_PIXEL_NV, GL_SHADING_RATE_1_INVOCATION_PER_2X2_PIXELS_NV,
    GL_SHADING_RATE_1_INVOCATION_PER_4X4_PIXELS_NV};

// Each eye's shading-rate image and what it was made for.
struct RateImage
{
    GLuint texture{0};
    int width{0}, height{0}; // the scene's, in pixels
    int tilesX{0}, tilesY{0};
    upscale::Lens lens;
    float inner{0.f}, outer{0.f};
};
RateImage images[2];

// The angles (degrees from the view axis) within which the shading is full rate, and 2x2; 4x4 beyond.
void radii(float& inner, float& outer)
{
    // conservative, balanced, aggressive
    constexpr float presets[3][2] = {{45.f, 60.f}, {35.f, 50.f}, {25.f, 40.f}};
    const int preset = za::clamp(static_cast<int>(vr_foveated.value), 1, 3) - 1;
    inner = vr_foveated_inner.value > 0.f ? vr_foveated_inner.value : presets[preset][0];
    outer = vr_foveated_outer.value > 0.f ? vr_foveated_outer.value : presets[preset][1];
    inner = za::clamp(inner, 5.f, 89.f);
    outer = za::clamp(outer, inner, 89.f);
}

[[nodiscard]] bool sameLens(const upscale::Lens& a, const upscale::Lens& b)
{
    return a.centre == b.centre && a.pixelsPerTangent == b.pixelsPerTangent;
}

// The eye's image for a width x height scene, made again when the size, the field of view or the radii change.
RateImage& rateImage(int eye, int width, int height)
{
    RateImage& img = images[eye & 1];
    const upscale::Lens lens = upscale::lens(eye, width, height);
    float inner = 0.f, outer = 0.f;
    radii(inner, outer);
    if(img.texture && img.width == width && img.height == height && sameLens(img.lens, lens) && img.inner == inner &&
        img.outer == outer)
    {
        return img;
    }

    const int tilesX = (width + tileWidth - 1) / tileWidth;
    const int tilesY = (height + tileHeight - 1) / tileHeight;
    if(!img.texture || img.tilesX != tilesX || img.tilesY != tilesY)
    {
        if(img.texture)
        {
            GL_DeleteNativeTexture(img.texture);
        }
        glGenTextures(1, &img.texture);
        GL_BindNative(GL_TEXTURE0, GL_TEXTURE_2D, img.texture);
        GL_TexStorage2DFunc(GL_TEXTURE_2D, 1, GL_R8UI, tilesX, tilesY);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        GL_ObjectLabelFunc(GL_TEXTURE, img.texture, -1, eye ? "vr shading rates R" : "vr shading rates L");
    }

    // Each tile's rate from its point nearest the lens centre (so no pixel is shaded coarser than its angle asks).
    const float tanInner = za::tan(glm::radians(inner));
    const float tanOuter = za::tan(glm::radians(outer));
    za::Vector<unsigned char> texels(static_cast<za::SizeT>(tilesX) * tilesY);
    double pixels[3] = {0.0, 0.0, 0.0};
    for(int ty = 0; ty < tilesY; ty++)
    {
        for(int tx = 0; tx < tilesX; tx++)
        {
            const float x0 = static_cast<float>(tx * tileWidth), y0 = static_cast<float>(ty * tileHeight);
            const float x1 = za::min(x0 + tileWidth, static_cast<float>(width));
            const float y1 = za::min(y0 + tileHeight, static_cast<float>(height));
            const glm::vec2 nearest{za::clamp(lens.centre.x, x0, x1), za::clamp(lens.centre.y, y0, y1)};
            const float t = glm::length((nearest - lens.centre) / lens.pixelsPerTangent);
            const unsigned char rate = t < tanInner ? 0 : t < tanOuter ? 1 : 2;
            texels[static_cast<za::SizeT>(ty) * tilesX + tx] = rate;
            pixels[rate] += static_cast<double>(x1 - x0) * (y1 - y0);
        }
    }
    GL_BindNative(GL_TEXTURE0, GL_TEXTURE_2D, img.texture);
    GLint alignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tilesX, tilesY, GL_RED_INTEGER, GL_UNSIGNED_BYTE, texels.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);

    img.width = width;
    img.height = height;
    img.tilesX = tilesX;
    img.tilesY = tilesY;
    img.lens = lens;
    img.inner = inner;
    img.outer = outer;

    if(vr_foveated_debug.value != 0.f)
    {
        const double all = static_cast<double>(width) * height;
        Con_Printf("VR: foveated %c eye %dx%d, %dx%d tiles, full rate within %.0f deg, 2x2 to %.0f: 1x1 %.0f%%, 2x2 "
                   "%.0f%%, 4x4 %.0f%% of the pixels; %.0f%% of the shading\n",
            eye ? 'R' : 'L', width, height, tileWidth, tileHeight, inner, outer, 100.0 * pixels[0] / all,
            100.0 * pixels[1] / all, 100.0 * pixels[2] / all,
            100.0 * (pixels[0] + pixels[1] / 4.0 + pixels[2] / 16.0) / all);
    }
    return img;
}

// While the scene renders: the framebuffer binding, watched, so the rates apply to the eye's scene framebuffers only.
using BindFramebufferFn = decltype(GL_BindFramebufferFunc);
BindFramebufferFn realBindFramebuffer = nullptr;
GLuint sceneFbos[2] = {0, 0};
bool active = false; // between beginScene and endScene
bool enabled = false; // GL_SHADING_RATE_IMAGE_NV

void setEnabled(bool on)
{
    if(on != enabled)
    {
        enabled = on;
        if(on)
        {
            glEnable(GL_SHADING_RATE_IMAGE_NV);
        }
        else
        {
            glDisable(GL_SHADING_RATE_IMAGE_NV);
        }
    }
}

void APIENTRY watchedBindFramebuffer(GLenum target, GLuint framebuffer)
{
    realBindFramebuffer(target, framebuffer);
    if(target != GL_READ_FRAMEBUFFER)
    {
        setEnabled(framebuffer != 0 && (framebuffer == sceneFbos[0] || framebuffer == sceneFbos[1]));
    }
}

GLuint debugProgram = 0;
bool debugFailed = false;

constexpr const char* debugVs = R"(#version 430
void main()
{
    ivec2 v = ivec2(gl_VertexID & 1, gl_VertexID >> 1);
    gl_Position = vec4(vec2(v) * 4.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* debugFs = R"(#version 430
layout(binding = 0) uniform usampler2D Rates;
layout(location = 0) uniform vec4 Map;  // scene pixels per image pixel (xy), 1 / the tile's size (zw)
layout(location = 1) uniform vec4 Lens; // the projection centre (image pixels), 1 / pixels per tangent
layout(location = 2) uniform vec2 Ring; // vr_upscale's radius and the ring's half width, as tangents (x 0: none)
layout(location = 3) uniform int HasRates;
layout(location = 4) uniform vec2 MaskRings; // the density mask's: the tangents of its inner and outer angles (y 0: none)
layout(location = 0) out vec4 Out;
void main()
{
    vec4 c = vec4(0.0);
    if(MaskRings.y > 0.0)
    {
        float t = length((gl_FragCoord.xy - Lens.xy) * Lens.zw);
        if(t >= MaskRings.y)
            c = vec4(1.0, 0.15, 0.0, 0.35);
        else if(t >= MaskRings.x)
            c = vec4(1.0, 0.9, 0.0, 0.22);
    }
    else if(HasRates != 0)
    {
        ivec2 t = clamp(ivec2(gl_FragCoord.xy * Map.xy * Map.zw), ivec2(0), textureSize(Rates, 0) - 1);
        uint r = texelFetch(Rates, t, 0).r;
        if(r == 1u)
            c = vec4(1.0, 0.9, 0.0, 0.22);
        else if(r >= 2u)
            c = vec4(1.0, 0.15, 0.0, 0.35);
    }
    if(Ring.x > 0.0 && abs(length((gl_FragCoord.xy - Lens.xy) * Lens.zw) - Ring.x) < Ring.y)
        c = vec4(0.0, 1.0, 1.0, 0.8);
    if(c.a <= 0.0)
        discard;
    Out = c;
}
)";

// The density mask (vr_foveated_mask). Its vertex shader: a full-screen triangle at the depth Depth (the hidden area's:
// the near plane).
constexpr const char* maskVs = R"(#version 430
layout(location = 3) uniform float Depth;
void main()
{
    ivec2 v = ivec2(gl_VertexID & 1, gl_VertexID >> 1);
    gl_Position = vec4(vec2(v) * 4.0 - 1.0, Depth, 1.0);
}
)";

// Which pixels are shaded, in the mask and its fill alike: by 2x2 blocks, each block's ring from its middle's angle
// off the view axis (the scene's pixels: gl_FragCoord), 0 all of them, 1 every other one (a checkerboard), 2 one in
// four (even x and y).
#define QVR_FOVEATED_KEPT \
    "layout(location = 0) uniform vec4 Lens; // the projection centre (the scene's pixels), 1 / pixels per tangent\n" \
    "layout(location = 1) uniform vec2 Rings; // the tangents of the inner and outer angles\n" \
    "int Ring(ivec2 b)\n" \
    "{\n" \
    "    float t = length((vec2(b) * 2.0 + 1.0 - Lens.xy) * Lens.zw);\n" \
    "    return t < Rings.x ? 0 : t < Rings.y ? 1 : 2;\n" \
    "}\n" \
    "bool Kept(ivec2 p)\n" \
    "{\n" \
    "    ivec2 b = p >> 1;\n" \
    "    int r = Ring(b);\n" \
    "    return r == 0 || (r == 1 ? ((b.x + b.y) & 1) == 0 : ((b.x | b.y) & 1) == 0);\n" \
    "}\n"

// The mask: the pixels left out, at the near plane (depth written; their colour black until filled).
constexpr const char* maskFs = "#version 430\n" QVR_FOVEATED_KEPT R"(
layout(location = 0) out vec4 Out;
void main()
{
    if(Kept(ivec2(gl_FragCoord.xy)))
        discard;
    Out = vec4(0.0);
}
)";

// The fill, into a single-sampled image of the scene (the scene itself without MSAA; with it, what it was resolved or
// copied into: the samples averaged already, and no multisampled colour to decompress and read): each pixel left out
// taking the nearest pixel of each of the 8 blocks round its own that are shaded (across them where half are, a
// checkerboard; across or at the corners where a quarter are), weighted by 1 / their distance. Reads only shaded
// pixels and writes only those left out: no feedback between them (after a glTextureBarrier for the draws before).
// Drawn at the mask's depth, depth-tested for equal where the scene's depth is attached (FillDepth: the shaded pixels
// not run at all), else over all of it.
constexpr const char* fillFs = "#version 430\n" QVR_FOVEATED_KEPT R"(
layout(binding = 0) uniform sampler2D Scene;
layout(location = 0) out vec4 Out;
const ivec2 directions[8] = ivec2[8](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1), ivec2(1, 1), ivec2(-1, 1),
    ivec2(1, -1), ivec2(-1, -1));
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    if(Kept(p))
        discard; // (shaded; at the near plane, a held gun's part drawn there: vr_nearclip_held)
    ivec2 last = textureSize(Scene, 0) - 1;
    ivec2 o = p & ~1; // its block's first pixel
    vec4 sum = vec4(0.0);
    float weights = 0.0;
    for(int i = 0; i < 8; i++)
    {
        // The block next to it this way: its pixel nearest to this one.
        ivec2 d = directions[i];
        ivec2 q = ivec2(d.x < 0 ? o.x - 1 : d.x > 0 ? o.x + 2 : p.x, d.y < 0 ? o.y - 1 : d.y > 0 ? o.y + 2 : p.y);
        if(any(lessThan(q, ivec2(0))) || any(greaterThan(q, last)) || !Kept(q))
            continue;
        float w = inversesqrt(float(dot(q - p, q - p)));
        sum += texelFetch(Scene, q, 0) * w;
        weights += w;
    }
    if(weights <= 0.0)
        discard;
    Out = sum / weights;
}
)";

using TextureBarrierFn = void(APIENTRY*)();
TextureBarrierFn textureBarrier = nullptr;
int maskSupport = -1; // unknown

GLuint maskProgram = 0;
bool maskFailed = false;
GLuint fillProgram = 0;
bool fillFailed = false;

// This scene's mask (beginScene to endScene).
struct Mask
{
    bool on{false};
    upscale::Lens lens;
    float tanInner{0.f}, tanOuter{0.f};
};
Mask mask;
Mask lastMask[2]; // each eye's last one, for vr_foveated_debug

// The hidden area's depth: the near plane (reversed Z with clip control, else -1 in clip space).
[[nodiscard]] float maskDepth()
{
    return gl_clipcontrol_able ? 1.f : -1.f;
}

void maskUniforms()
{
    GL_Uniform4fFunc(0, mask.lens.centre.x, mask.lens.centre.y, 1.f / mask.lens.pixelsPerTangent.x,
        1.f / mask.lens.pixelsPerTangent.y);
    GL_Uniform2fFunc(1, mask.tanInner, mask.tanOuter);
    GL_Uniform1fFunc(3, maskDepth());
}

[[nodiscard]] bool ensureFillProgram()
{
    if(!fillProgram && !fillFailed)
    {
        fillProgram = gfx::glProgram(maskVs, fillFs, "vr foveated fill");
        fillFailed = !fillProgram;
    }
    return fillProgram != 0;
}

[[nodiscard]] bool ensureMaskProgram()
{
    if(!maskProgram && !maskFailed)
    {
        maskProgram = gfx::glProgram(maskVs, maskFs, "vr foveated mask");
        maskFailed = !maskProgram;
    }
    return maskProgram != 0;
}

// Whether an eye's own scene of `width` x `height` gets the density mask (vr_foveated_mask: without variable-rate
// shading, or 2; not in a view through a teleporter: its pixels are read through the gate's, and are not filled).
[[nodiscard]] bool maskWanted(int width, int height)
{
    if(vr_foveated.value < 1.f || r_refdef.scale != 1 || width <= 0 || height <= 0 || VR_PortalDrawing())
    {
        return false;
    }
    const int mode = static_cast<int>(vr_foveated_mask.value);
    return (mode >= 2 || (mode >= 1 && !supported())) && maskSupported() && ensureMaskProgram() && ensureFillProgram();
}

// The eye's mask: its lens and rings.
[[nodiscard]] Mask maskFor(int eye, int width, int height)
{
    float inner = 0.f, outer = 0.f;
    radii(inner, outer);
    Mask m;
    m.on = true;
    m.lens = upscale::lens(eye, width, height);
    m.tanInner = za::tan(glm::radians(inner));
    m.tanOuter = za::tan(glm::radians(outer));
    return m;
}

// The mask over what is bound (the eye's scene, just cleared, its hidden area drawn).
void drawMask(int eye, int width, int height)
{
    mask = maskFor(eye, width, height);
    lastMask[eye & 1] = mask;

    QVR_GPU_PROFILE("foveation mask");
    GL_UseProgram(maskProgram);
    GL_DepthRange(ZRANGE_FULL);
    GL_SetState(GLS_BLEND_OPAQUE | GLS_CULL_NONE | GLS_ATTRIBS(0)); // depth tested (passes against the clear) and written
    maskUniforms();
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

} // namespace

bool maskSupported()
{
    if(maskSupport >= 0)
    {
        return maskSupport != 0;
    }
    textureBarrier = reinterpret_cast<TextureBarrierFn>(SDL_GL_GetProcAddress("glTextureBarrier"));
    if(!textureBarrier)
    {
        textureBarrier = reinterpret_cast<TextureBarrierFn>(SDL_GL_GetProcAddress("glTextureBarrierNV"));
    }
    maskSupport = textureBarrier ? 1 : 0;
    Con_Printf("VR: foveated rendering's density mask %s\n", maskSupport ? "available" : "not available (no glTextureBarrier)");
    return maskSupport != 0;
}

bool masking()
{
    return mask.on;
}

namespace
{

// The fill into `fbo` (its colour `texture`, single-sampled, the scene's pixels at the same places) over `viewport`;
// `depthTested`: the scene's depth attached, only the mask's depth shaded.
void fillInto(GLuint fbo, GLuint texture, const int viewport[4], bool depthTested)
{
    QVR_GPU_PROFILE("foveation fill");
    GL_BindFramebufferFunc(GL_FRAMEBUFFER, fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    textureBarrier(); // the draws or the blit into it, read now
    GL_UseProgram(fillProgram);
    GL_DepthRange(ZRANGE_FULL);
    GL_SetState(GLS_BLEND_OPAQUE | GLS_CULL_NONE | GLS_ATTRIBS(0) | GLS_NO_ZWRITE | (depthTested ? 0 : GLS_NO_ZTEST));
    if(depthTested)
    {
        glDepthFunc(GL_EQUAL); // the mask's pixels only: the rest not shaded at all
    }
    GL_BindNative(GL_TEXTURE0, GL_TEXTURE_2D, texture);
    maskUniforms();
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if(depthTested)
    {
        glDepthFunc(gl_clipcontrol_able ? GL_GEQUAL : GL_LEQUAL); // GL_SetupState's
    }
}

} // namespace

void fillMasked()
{
    GLuint color = 0, depth = 0;
    int samples = 1, viewport[4]{};
    const GLuint fbo = mask.on ? VR_SceneTarget(&color, &depth, &samples, viewport) : 0;
    if(fbo && color && samples <= 1)
    {
        fillInto(fbo, color, viewport, true);
    }
}

void fillResolved(GLuint fbo, GLuint texture, const int viewport[4])
{
    if(mask.on && fbo && texture)
    {
        fillInto(fbo, texture, viewport, false);
    }
}

bool supported()
{
    if(support >= 0)
    {
        return support != 0;
    }
    support = 0;
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    bool found = false;
    for(GLint i = 0; i < count && !found; i++)
    {
        const auto* name = reinterpret_cast<const char*>(GL_GetStringiFunc(GL_EXTENSIONS, static_cast<GLuint>(i)));
        found = name && !ZA_STRCMP(name, "GL_NV_shading_rate_image");
    }
    if(found)
    {
        bindShadingRateImage =
            reinterpret_cast<BindShadingRateImageFn>(SDL_GL_GetProcAddress("glBindShadingRateImageNV"));
        shadingRateImagePalette =
            reinterpret_cast<ShadingRateImagePaletteFn>(SDL_GL_GetProcAddress("glShadingRateImagePaletteNV"));
        GLint w = 0, h = 0;
        glGetIntegerv(GL_SHADING_RATE_IMAGE_TEXEL_WIDTH_NV, &w);
        glGetIntegerv(GL_SHADING_RATE_IMAGE_TEXEL_HEIGHT_NV, &h);
        if(bindShadingRateImage && shadingRateImagePalette && w > 0 && h > 0)
        {
            tileWidth = w;
            tileHeight = h;
            support = 1;
        }
    }
    Con_Printf("VR: foveated rendering (GL_NV_shading_rate_image) %s", support ? "available" : "not available\n");
    if(support)
    {
        Con_Printf(", %dx%d pixel tiles\n", tileWidth, tileHeight);
    }
    return support != 0;
}

void beginScene(int eye, int width, int height)
{
    endScene(); // one left open (an error during the last eye's scene)
    lastMask[eye & 1].on = false;
    if(vr_foveated.value < 1.f || r_refdef.scale != 1 || width <= 0 || height <= 0)
    {
        return;
    }
    // Without variable-rate shading (or vr_foveated_mask 2): the density mask.
    const int maskMode = static_cast<int>(vr_foveated_mask.value);
    if(maskMode >= 2 || (maskMode >= 1 && !supported()))
    {
        if(maskWanted(width, height))
        {
            drawMask(eye, width, height);
        }
        return;
    }
    if(!supported())
    {
        return;
    }
    QVR_PROFILE("foveation");
    const RateImage& img = rateImage(eye, width, height);
    bindShadingRateImage(img.texture);
    shadingRateImagePalette(0, 0, static_cast<GLsizei>(za::getArraySize(palette)), palette);

    if(GL_NeedsSceneEffects())
    {
        sceneFbos[0] = framebufs.scene.fbo;
        sceneFbos[1] = framebufs.oit.fbo_scene;
    }
    else
    {
        sceneFbos[0] = framebufs.composite.fbo;
        sceneFbos[1] = framebufs.oit.fbo_composite;
    }
    if(GL_BindFramebufferFunc != watchedBindFramebuffer)
    {
        realBindFramebuffer = GL_BindFramebufferFunc;
        GL_BindFramebufferFunc = watchedBindFramebuffer;
    }
    active = true;
    // What is bound: R_SetupGL's target, as R_RenderScene has just bound it (VR_DrawHiddenArea, after R_Clear). Not read
    // back: a glGet waits for the driver's thread, 0.2-0.4 ms of the CPU an eye (the profiler's "foveation").
    GLuint color = 0, depth = 0;
    int samples = 1;
    int viewport[4]{};
    const GLuint bound = VR_SceneTarget(&color, &depth, &samples, viewport);
    setEnabled(bound != 0 && (bound == sceneFbos[0] || bound == sceneFbos[1]));
}

void shadeBoundAsScene(bool on)
{
    setEnabled(on && active);
}

void endScene()
{
    mask.on = false;
    if(!active)
    {
        return;
    }
    active = false;
    setEnabled(false);
    if(GL_BindFramebufferFunc == watchedBindFramebuffer)
    {
        GL_BindFramebufferFunc = realBindFramebuffer;
    }
    bindShadingRateImage(0);
}

void drawDebug(int eye, GLuint fbo, int width, int height, int sceneWidth, int sceneHeight)
{
    if(vr_foveated_debug.value == 0.f)
    {
        return;
    }
    if(!debugProgram && !debugFailed)
    {
        debugProgram = gfx::glProgram(debugVs, debugFs, "vr foveated debug");
        debugFailed = !debugProgram;
    }
    if(!debugProgram)
    {
        return;
    }
    const RateImage& img = images[eye & 1];
    const Mask& eyeMask = lastMask[eye & 1];
    const bool masked = vr_foveated.value >= 1.f && eyeMask.on;
    const bool hasRates = !masked && vr_foveated.value >= 1.f && img.texture && img.width == sceneWidth && img.height == sceneHeight;
    const upscale::Lens lens = upscale::lens(eye, width, height);
    const bool ring = vr_upscale.value >= 1.f && upscale::radiusTangent() > 0.f;

    GL_BindFramebufferFunc(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    GL_SetState(GLS_BLEND_ALPHA | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(0));
    GL_UseProgram(debugProgram);
    GL_BindNative(GL_TEXTURE0, GL_TEXTURE_2D, hasRates ? img.texture : 0);
    GL_Uniform4fFunc(0, static_cast<float>(sceneWidth) / width, static_cast<float>(sceneHeight) / height,
        1.f / tileWidth, 1.f / tileHeight);
    GL_Uniform4fFunc(1, lens.centre.x, lens.centre.y, 1.f / lens.pixelsPerTangent.x, 1.f / lens.pixelsPerTangent.y);
    GL_Uniform2fFunc(2, ring ? upscale::radiusTangent() : 0.f, 1.5f / lens.pixelsPerTangent.x);
    GL_Uniform1iFunc(3, hasRates ? 1 : 0);
    GL_Uniform2fFunc(4, masked ? eyeMask.tanInner : 0.f, masked ? eyeMask.tanOuter : 0.f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

} // namespace qvr::foveated
