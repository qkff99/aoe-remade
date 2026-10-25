#pragma once

#include <string>

// Hardware instancing for grass (details). The stock, SSS and mod grass vertex shaders all read their instance
// from constant arrays: `int i = v.misc.w; array[i + 0..3]` (and optionally `exdata[i / 4]`). Instead of shipping
// replacement shaders, the engine derives an instanced twin of whichever shader is installed: the original entry
// is kept, those arrays become statics, and a generated `main` fills them from a per-instance vertex stream.
namespace detail_instancing
{
// Shader resource names carrying this option are compiled through Adapt().
constexpr const char* option = "AOE_DETAIL_INSTANCING";
constexpr const char* resource_suffix = "(AOE_DETAIL_INSTANCING,)";

// Per-instance record in vertex stream 1 (TEXCOORD1..5).
struct Instance
{
    float m0[4], m1[4], m2[4];   // world matrix rows, translation in w (as in the legacy constant array)
    float normal_alpha[4];       // terrain normal, blade alpha
    float sun_hemi_distance[4];  // sun, hemi, squared camera distance of the slot
};
static_assert(sizeof(Instance) == 80, "Instance must match the input layout declared by CDetailManager");

// True when the source follows the stock contract; `out` then holds the instanced source.
bool Adapt(const char* source, size_t size, std::string& out);
}
