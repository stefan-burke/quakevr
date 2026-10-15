// vr_foveated.hpp -- fixed foveated rendering (vr_foveated): the eye's scene shaded at full rate near the lens centre,
// less further out and least at the edge. Two ways:
//
// - NVIDIA's variable-rate shading (GL_NV_shading_rate_image): once per 2x2 pixels further out, once per 4x4 at the
//   edge. Each eye has its own shading-rate image (the lenses' centres are mirrored), one texel per tile of the size
//   the driver gives, its rate from the angle between the view axis and the tile's nearest point.
// - Elsewhere (AMD, Intel, the Steam Frame's Adreno: any GL 4.5), the density mask (vr_foveated_mask; Valve's radial
//   density masking): the same rings, every other 2x2 block of pixels (a checkerboard) left out further out and three
//   in four at the edge, by a mask at the near plane drawn right after the lenses' hidden area (every later
//   depth-tested draw fails there, the depth pre-pass too; not before shading in the world's programs that write
//   their depth, vr_parallax_depth_write's: the drivers measured shade those pixels anyway), the blocks left out then
//   filled from the shaded pixels round them, in what reads the scene's colour back (the liquids' refraction, the hot
//   air's copy) and at the scene's end (fillMasked in place; with MSAA, fillResolved in the single-sampled resolve).
//   The pixels left out keep the mask's depth, so what is drawn after a fill skips them still (and the next fill
//   takes them from it).
//
// Either covers the scene: the world, models, liquids, particles, the hands and weapons (from right after the scene's
// clear to the end of the eye's V_RenderView). The rates apply only while one of the eye's own scene framebuffers is
// bound (a pass into a texture of another size, such as the water's scene distances or a screen's canvas, shades every
// pixel). Not the post-processing, bloom, the upscale or the UI. Not the views through teleporters (the mask; the
// rates as before). The depth pre-pass has no fragment shader: the rates change nothing there.

#pragma once

#include "vr_engine.hpp"

namespace qvr::foveated
{

// Whether the driver has GL_NV_shading_rate_image (checked once).
[[nodiscard]] bool supported();

// Whether the density mask can be drawn and filled (glTextureBarrier: GL 4.5 or ARB/NV_texture_barrier; checked once).
[[nodiscard]] bool maskSupported();

// The eye's scene begins (its framebuffer bound, after the clear and the lenses' hidden area): variable-rate shading
// on for it, or the density mask drawn, if vr_foveated asks. `width` x `height`: the scene's size.
void beginScene(int eye, int width, int height);

// The eye's scene is over: variable-rate shading off, the density mask done with.
void endScene();

// Whether this scene has the density mask (between beginScene and endScene).
[[nodiscard]] bool masking();

// The density mask's pixels left out filled from the shaded ones round them, in the scene's colour in place
// (VR_SceneTarget's framebuffer: bound after). Nothing without the mask, or with MSAA: then fillResolved, on what the
// scene is resolved or copied into.
void fillMasked();

// The same in a single-sampled image of the scene (its colour `texture`, attached to `fbo`: bound after; the scene's
// pixels at the same places) over `viewport`: a multisampled scene's resolve, a copy of the scene.
void fillResolved(GLuint fbo, GLuint texture, const int viewport[4]);

// What is bound now (until the next framebuffer bind) shaded as the eye's scene is (`on`: variable-rate while the
// scene's is; a target of the scene's size whose pixels are the scene's, as vr_gfx_gl.cpp's particles composited
// apart) or at full rate (`on` false: a pass copying pixels into the scene's own framebuffer).
void shadeBoundAsScene(bool on);

// vr_foveated_debug: the rates (yellow 2x2 or half the blocks, red 4x4 or a quarter) and vr_upscale's circle (cyan)
// over the eye's image `fbo` (width x height), the scene having been sceneWidth x sceneHeight.
void drawDebug(int eye, GLuint fbo, int width, int height, int sceneWidth, int sceneHeight);

} // namespace qvr::foveated
