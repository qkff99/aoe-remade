#pragma once

// GPU side of r4_detail_instancing 2: grass records stay resident in a GPU arena and a compute shader expands the ones
// visible this frame into the instance stream. The compute shader is embedded (DetailGatherCodec.h).
#include <d3d11.h>

namespace detail_gather
{
struct Buffer
{
    ID3D11Buffer* buffer = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    u32 capacity = 0; // elements of the owner's stride
    void Release();
};

struct Gatherer
{
    ID3D11ComputeShader* shader = nullptr;
    bool failed = false;
    Buffer arena, transient, ranges; // 32-byte records, 32-byte records, 16-byte entries
    u32 arena_used = 0; // records
    u32 arena_wanted = 0; // records, honoured at the start of the next frame
    u32 epoch = 1; // changes whenever the arena content is thrown away
    ID3D11Buffer* output_buffer = nullptr; // buffer the UAV below belongs to
    ID3D11UnorderedAccessView* output_uav = nullptr;

    bool Init();
    ~Gatherer();
    void ResetOutput();
};

// Creates the gatherer on first use; null when this device cannot run it.
Gatherer* Acquire(Gatherer*& slot);

// r4_detail_thin: the share of a slot's instances that is drawn falls linearly from 1 at `start` metres to `keep` at
// `start + 1 / inv_range`; the gather shader widens the blades that remain by 1 / sqrt(share) to keep the coverage.
struct Thin
{
    float start, inv_range, keep;
};
Thin ReadThin();

// full * share instances are drawn: whole ones, and a last one that grows from nothing as the share rises, so that blades
// fade in instead of popping. `tip` is that last blade's scale in 1/65535.
struct Thinned
{
    u32 count, tip;
};
Thinned ThinnedCount(u32 full, float distance_sq, const Thin& thin);
}
