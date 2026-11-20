// Runs on every generated frame after the generator (FgGuard), in place.
// 1. Hold (FgStats): the nearest real frame instead of a generated one nothing could explain.
// 2. Global motion: where the two real frames agree once moved by the pair's global motion, the
//    picture at t is their motion-compensated blend, exact for a camera pan; elsewhere (parallax,
//    moving objects, uncovered borders) the generator's frame stays. Both read the frames without
//    overlays, so no overlay is moved with the scene.
// 3. Overlays: the static-overlay mask takes the real frames' own pixels, blended at t.
#include "FgGuard.hlsli"
Texture2D<float4> previousScene : register(t0);   // real A, overlays removed (or A itself)
Texture2D<float4> currentScene : register(t1);    // real B, overlays removed (or B itself)
Texture2D<float4> previousColor : register(t2);   // real A
Texture2D<float4> currentColor : register(t3);    // real B
Texture2D<float> score : register(t4);
Texture2D<float4> stats : register(t5);
RWTexture2D<float4> generated : register(u0);
SamplerState linearClamp : register(s1);

cbuffer FgFinalizeConstants : register(b0)
{
    uint width;
    uint height;
    float t;              // position of this frame between A (0) and B (1)
    uint flags;           // 1 overlays, 2 motion guard, 4 paint the overlay mask (VEYRA_TEST_FG_GUARD_VIEW)
    float agreeLimit;     // the two moved frames agree below this mean difference
    uint unused[3];
};

float difference(float3 a, float3 b)
{
    const float3 d = abs(a - b);
    return max(d.r, max(d.g, d.b));
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    const int2 p = int2(id.xy);
    const int2 limit = int2(width - 1, height - 1);
    const uint decision = (flags & 2u) != 0 ? uint(stats[uint2(1, 0)].z) : 0u;
    if (decision & kFgHold) {
        generated[p] = t < 0.5 ? previousColor[p] : currentColor[p];
        return;
    }
    float4 result = generated[p];
    if (decision & kFgGlobal) {
        // A at q + t*m and B at q - (1-t)*m show what is at q at time t (m: current->previous).
        const float2 m = stats[uint2(0, 0)].xy;
        const float2 size = float2(width, height);
        const float2 q = p + 0.5;
        const float2 qa = q + t * m, qb = q - (1.0 - t) * m;
        const bool va = all(qa >= 0.5) && all(qa <= size - 0.5), vb = all(qb >= 0.5) && all(qb <= size - 0.5);
        if (va || vb) {
            const float4 a = previousScene.SampleLevel(linearClamp, qa / size, 0);
            const float4 b = currentScene.SampleLevel(linearClamp, qb / size, 0);
            const float wa = va ? 1.0 - t : 0.0, wb = vb ? t : 0.0;
            const float4 blend = (a * wa + b * wb) / (wa + wb);
            float weight = 1.0;
            if (va && vb) {
                // Agreement over a sparse 3x3 neighbourhood, 4 pixels apart. Screen-static
                // content the overlay mask does not cover (inside a minimap, a weapon) agrees
                // better unmoved: the generator keeps it.
                float disagree = 0, still = 0;
                [unroll] for (int j = 0; j < 9; ++j) {
                    const float2 o = float2(j % 3 - 1, j / 3 - 1) * 4.0;
                    disagree += difference(previousScene.SampleLevel(linearClamp, (qa + o) / size, 0).rgb,
                                           currentScene.SampleLevel(linearClamp, (qb + o) / size, 0).rgb);
                    const int2 r = clamp(p + int2(o), int2(0, 0), limit);
                    still += difference(previousScene[r].rgb, currentScene[r].rgb);
                }
                weight = still < 0.5 * disagree ? 0.0 : saturate(2.0 - disagree / 9.0 / agreeLimit);
            }
            result = lerp(result, blend, weight);
        }
    }
    if (flags & 1u) {
        // The mask, and the band FgHudInpaint also took out of the generator's input where the
        // pixel did not change (the rest of a glyph the mask missed, an outline).
        float m = HudMaskDilated(score, p, limit);
        if (m < 1 && difference(previousColor[p].rgb, currentColor[p].rgb) < 0.06) {
            [unroll] for (int j = 0; j < 25; ++j)
                m = max(m, HudMask(score[clamp(p + int2(j % 5 - 2, j / 5 - 2), int2(0, 0), limit)]));
        }
        if (m > 0) result = lerp(result, (flags & 4u) ? float4(1, 0, 1, 1) : lerp(previousColor[p], currentColor[p], t), m);
    }
    generated[p] = result;
}
