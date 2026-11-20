// Static screen overlays (crosshair, HUD text, minimap) for frame generation (FgGuard).
// A pixel is screen-static when its 3x3 patch is the same in the previous and the current real
// frame while the scene's motion there does NOT explain it: the optical flow gives an overlay
// the motion of the scene behind it, and the patch that motion points to in the previous frame
// is scene, not overlay. Smooth scenery that moved matches both ways and is left alone. Each such
// pair raises the pixel's score, a clear change lowers it, so one coincidence masks nothing.
// The centre box (crosshairs) takes a looser static test and lets go more slowly, so a crosshair
// that fades in and out while aiming stays masked across the fade.
Texture2D<float4> previousColor : register(t0);
Texture2D<float4> currentColor : register(t1);
Texture2D<float2> flow : register(t2);          // current->previous work pixels
RWTexture2D<float> score : register(u0);
SamplerState linearClamp : register(s1);

cbuffer FgHudDetectConstants : register(b0)
{
    uint width;
    uint height;
    uint centreX0, centreY0, centreX1, centreY1;
    uint reset;
    uint haveFlow;
};

float difference(float3 a, float3 b)
{
    const float3 d = abs(a - b);
    return max(d.r, max(d.g, d.b));
}

// Mean 3x3 difference between the current patch at p and the previous patch moved by m.
float patchCost(int2 p, float2 m, float3 current[9], float2 size)
{
    float cost = 0;
    [unroll] for (int j = 0; j < 9; ++j) {
        const float2 q = p + 0.5 + float2(j % 3 - 1, j / 3 - 1) + m;
        cost += difference(current[j], previousColor.SampleLevel(linearClamp, q / size, 0).rgb);
    }
    return cost / 9.0;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    const int2 p = int2(id.xy);
    if (reset != 0 || haveFlow == 0) { if (reset != 0) score[p] = 0; return; }
    const int2 limit = int2(width - 1, height - 1);
    const float2 size = float2(width, height);
    float3 current[9];
    float still = 0;
    [unroll] for (int j = 0; j < 9; ++j) {
        const int2 q = clamp(p + int2(j % 3 - 1, j / 3 - 1), int2(0, 0), limit);
        current[j] = currentColor[q].rgb;
        still = max(still, difference(current[j], previousColor[q].rgb));
    }
    // The scene's motion here: this pixel's vector, and the fastest of 8 around it (an overlay
    // big enough to hold its own block of the flow gets the scene's motion from beside it).
    const float2 own = flow[p];
    float2 around = own;
    [unroll] for (int k = 0; k < 8; ++k) {
        const float angle = k * 0.785398;
        const float2 v = flow[clamp(p + int2(round(float2(cos(angle), sin(angle)) * 24.0)), int2(0, 0), limit)];
        if (dot(v, v) > dot(around, around)) around = v;
    }
    float s = score[p];
    if (dot(around, around) >= 2.25) {
        const float moved = min(patchCost(p, own, current, size), patchCost(p, around, current, size));
        const bool centre = all(p >= int2(centreX0, centreY0)) && all(p < int2(centreX1, centreY1));
        const bool isStatic = moved > 0.08 && still < 0.03 + (centre ? 0.4 : 0.25) * moved;
        if (isStatic) s = min(1.0, s + 0.34);
        else if (still > (centre ? 0.3 : 0.12)) s = max(0.0, s - (centre ? 0.2 : 0.5));
    }
    score[p] = s;
}
