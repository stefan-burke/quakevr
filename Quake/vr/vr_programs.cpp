// vr_programs.cpp -- which of the world and model shaders' optional features a frame needs (VR_WorldFeatures,
// VR_AliasFeatures), for the programs made with only those compiled in (gl_shaders.c: GL_WorldProgram,
// GL_AliasProgram).

#include "vr_cvars.hpp"
#include "vr_engine.hpp"

using namespace qvr;

namespace
{

struct LightShadows
{
    bool mapped{false}; // a light has a shadow map (the models' shadows)
    bool any{false};    // ... or any shadow, or lights through a teleporter (the world's shadow functions' work)
};

// The lights' shadows as the light buffer has them at this moment, read anew each call: the buffer is not once
// a frame. Every view builds it again (R_PushDlights), and the shadow maps' pass reads the last view's before
// this view's is built, so an answer kept for a frame would be the first reader's — the shadow pass's, of the
// previous buffer — for the frame's draws. A walk of at most MAX_DLIGHTS lights, against the draw it chooses a
// program for.
[[nodiscard]] LightShadows lightShadows()
{
    LightShadows shadows;
    for(int i = 0; i < r_framedata.numlights; i++)
    {
        const gpulight_t& l = r_lightbuffer.lights[i];
        shadows.mapped |= l.shadow[2] > 0.f;
        shadows.any |= l.shadow[2] > 0.f || l.shadow[3] != 0.f || l.gatelo[3] > 0.f;
    }
    return shadows;
}

} // namespace

// Each feature as the world shader's own branch tests it: the frame's data (set before the world is drawn), the
// lights, or the cvars the instances' values come from.
extern "C" unsigned VR_WorldFeatures(void)
{
    unsigned f = 0;
    if(VR_RetroWorldUsed(0)) // (the liquids' program asks for theirs: r_world.c)
    {
        f |= QVR_WF_RETRO;
    }
    if(r_framedata.retrolight[2][3] > 0.f)
    {
        f |= QVR_WF_RETROLIGHT;
    }
    if(r_framedata.decalclock[2] > 0.f)
    {
        f |= QVR_WF_DECALS;
    }
    if(r_framedata.lighttweak[3] > 0.f || r_framedata.lighttweak[2] > 0.f)
    {
        f |= QVR_WF_NORMALS;
    }
    if(vr_parallax.value != 0.f && vr_normalmaps.value != 0.f)
    {
        f |= QVR_WF_PARALLAX;
    }
    if(r_framedata.detail[0] > 0.f)
    {
        f |= QVR_WF_DETAIL;
    }
    if(lightShadows().any)
    {
        f |= QVR_WF_SHADOWS;
    }
    return f;
}

// The same for the model shader.
extern "C" unsigned VR_AliasFeatures(void)
{
    unsigned f = 0;
    if(vr_retro.value != 0.f)
    {
        f |= QVR_AF_RETRO;
    }
    if(r_framedata.retrolight[5][2] > 0.f)
    {
        f |= QVR_AF_RETROLIGHT;
    }
    if(vr_parallax.value != 0.f && vr_normalmaps.value != 0.f)
    {
        f |= QVR_AF_PARALLAX;
    }
    if(vr_normalmaps.value != 0.f)
    {
        f |= QVR_AF_NORMALS;
    }
    if((r_framedata.shadowflags & 8) != 0 && r_framedata.numlights > 0)
    {
        f |= QVR_AF_DLIGHTS;
        if(lightShadows().mapped)
        {
            f |= QVR_AF_SHADOWS;
        }
    }
    if(r_framedata.lighttweak[2] > 0.f)
    {
        f |= QVR_AF_SPECULAR;
    }
    if(vr_rim_light.value > 0.f || (vr_weapon_reflections.value > 0.f && vr_weapon_reflections_strength.value > 0.f))
    {
        f |= QVR_AF_RIM;
    }
    return f;
}
