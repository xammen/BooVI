// One thread group: the pair's global motion and how well it, and the per-pixel flow, explain
// the current frame (FgGuard). The global motion is the most common flow vector (64x64 bins of
// 4 pixels over 16384 samples), refined to the mean of the vectors in and next to that bin.
// texel 0: global motion xy (current->previous work pixels), share of samples near it, share the
//          global motion explains photometrically
// texel 1: share the per-pixel flow explains, share that changed at all, decision bits, speed
#include "FgGuard.hlsli"
Texture2D<float4> previousColor : register(t0);
Texture2D<float4> currentColor : register(t1);
Texture2D<float2> flow : register(t2);
RWTexture2D<float4> stats : register(u0);
SamplerState linearClamp : register(s1);

cbuffer FgStatsConstants : register(b0)
{
    uint width;
    uint height;
    uint flags;           // 1 hold allowed, 2 global blend allowed, 4 flow valid
    float minSpeed;       // global blend from this many pixels a pair
    float explainLimit;   // a sample is explained under this colour difference
    uint unused[3];
};

groupshared uint bins[4096];
groupshared uint best;
groupshared int sumX, sumY;
groupshared uint near, explainedGlobal, explainedFlow, changed;

uint binOf(float2 v)
{
    const int2 b = int2(floor(v / 4.0)) + 32;
    return any(b < 1) || any(b > 62) ? 0xffffffffu : uint(b.y * 64 + b.x);
}

float difference(float3 a, float3 b)
{
    const float3 d = abs(a - b);
    return max(d.r, max(d.g, d.b));
}

int2 samplePoint(uint s, uint columns, uint rows)
{
    return int2((float2(s % columns, s / columns) + 0.5) * float2(width, height) / float2(columns, rows));
}

[numthreads(1024, 1, 1)]
void main(uint gi : SV_GroupIndex)
{
    for (uint i = gi; i < 4096; i += 1024) bins[i] = 0;
    if (gi == 0) { best = 0; sumX = 0; sumY = 0; near = 0; explainedGlobal = 0; explainedFlow = 0; changed = 0; }
    GroupMemoryBarrierWithGroupSync();
    const bool haveFlow = (flags & 4u) != 0;
    if (haveFlow) {
        for (uint a = gi; a < 16384; a += 1024) {
            const uint b = binOf(flow[samplePoint(a, 128, 128)]);
            if (b != 0xffffffffu) InterlockedAdd(bins[b], 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = gi; k < 4096; k += 1024) InterlockedMax(best, (min(bins[k], 0xfffffu) << 12) | k);
    GroupMemoryBarrierWithGroupSync();
    const int2 winner = int2((best & 4095u) % 64, (best & 4095u) / 64);
    if (haveFlow) {
        for (uint c = gi; c < 16384; c += 1024) {
            const float2 v = flow[samplePoint(c, 128, 128)];
            const uint b = binOf(v);
            if (b == 0xffffffffu || any(abs(int2(b % 64, b / 64) - winner) > 1)) continue;
            InterlockedAdd(sumX, int(round(v.x * 16.0)));
            InterlockedAdd(sumY, int(round(v.y * 16.0)));
            InterlockedAdd(near, 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const float2 global = near > 0 ? float2(sumX, sumY) / (16.0 * near) : 0;
    const float2 size = float2(width, height);
    for (uint e = gi; e < 4096; e += 1024) {
        const int2 p = samplePoint(e, 64, 64);
        const float3 current = currentColor[p].rgb;
        const float2 g = p + 0.5 + global;
        if (all(g >= 0) && all(g <= size) && difference(current, previousColor.SampleLevel(linearClamp, g / size, 0).rgb) < explainLimit)
            InterlockedAdd(explainedGlobal, 1);
        const float2 f = p + 0.5 + (haveFlow ? flow[p] : 0);
        if (difference(current, previousColor.SampleLevel(linearClamp, clamp(f, 0, size) / size, 0).rgb) < explainLimit)
            InterlockedAdd(explainedFlow, 1);
        if (difference(current, previousColor[p].rgb) >= explainLimit) InterlockedAdd(changed, 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi != 0) return;
    const float share = near / 16384.0;
    const float byGlobal = explainedGlobal / 4096.0, byFlow = explainedFlow / 4096.0, moved = changed / 4096.0;
    const float speed = length(global);
    uint decision = 0;
    // Most of the picture changed and no motion is common to it: a flick past the flow's reach
    // or a cut. (The per-pixel flow still "explains" much of a flick photometrically, on smooth
    // scenery, so it is no test.) The nearest real frame is exact where a generated one would
    // smear.
    if ((flags & 1u) && haveFlow && moved > 0.5 && share < 0.15 && byGlobal < 0.4) decision |= kFgHold;
    else if ((flags & 2u) && haveFlow && speed >= minSpeed && share >= 0.25 && byGlobal >= 0.3) decision |= kFgGlobal;
    stats[uint2(0, 0)] = float4(global, share, byGlobal);
    stats[uint2(1, 0)] = float4(byFlow, moved, decision, speed);
}
