// vr_programs.cpp -- which of the world and model shaders' optional features a frame needs (VR_WorldFeatures), for
// the programs made with only those compiled in (gl_shaders.c: GL_WorldProgram).

#include "vr_cvars.hpp"
#include "vr_engine.hpp"

using namespace qvr;

namespace
{

// Whether a light this frame has a shadow, or lights through a teleporter (the shadow functions' work): once a frame.
[[nodiscard]] bool lightsShadowed()
{
    static int frame = -1;
    static bool shadowed = false;
    if(frame != host_framecount)
    {
        frame = host_framecount;
        shadowed = false;
        for(int i = 0; i < r_framedata.numlights && !shadowed; i++)
        {
            const gpulight_t& l = r_lightbuffer.lights[i];
            shadowed = l.shadow[2] > 0.f || l.shadow[3] != 0.f || l.gatelo[3] > 0.f;
        }
    }
    return shadowed;
}

} // namespace

// Each feature as the world shader's own branch tests it: the frame's data (set before the world is drawn), the
// lights, or the cvars the instances' values come from.
extern "C" unsigned VR_WorldFeatures(void)
{
    unsigned f = 0;
    if(vr_retro.value != 0.f)
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
    if(lightsShadowed())
    {
        f |= QVR_WF_SHADOWS;
    }
    return f;
}
