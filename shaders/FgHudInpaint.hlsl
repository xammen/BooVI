// The frame-generation input without its static overlays (FgGuard): masked pixels take the
// scene around them, so the generator never moves a crosshair or HUD text with the background.
// FgFinalize puts the overlay back on every generated frame.
#include "FgGuard.hlsli"
Texture2D<float4> currentColor : register(t0);
Texture2D<float> score : register(t1);
Texture2D<float4> previousColor : register(t2);
RWTexture2D<float4> hudless : register(u0);

cbuffer FgHudInpaintConstants : register(b0)
{
    uint width;
    uint height;
    uint unused[6];
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    const int2 p = int2(id.xy);
    const int2 limit = int2(width - 1, height - 1);
    const float4 colour = currentColor[p];
    // The mask, and two pixels around it where the pixel did not change (an overlay's soft edge,
    // outline, the rest of a glyph the mask missed); the scene seen through a glyph stays.
    // FgFinalize restores exactly this set.
    float m = HudMaskDilated(score, p, limit);
    if (m < 1) {
        const float3 d = abs(previousColor[p].rgb - colour.rgb);
        if (max(d.r, max(d.g, d.b)) < 0.06) {
            [unroll] for (int j = 0; j < 25; ++j)
                m = max(m, HudMask(score[clamp(p + int2(j % 5 - 2, j / 5 - 2), int2(0, 0), limit)]));
        }
    }
    if (m <= 0) { hudless[p] = colour; return; }
    // 16 directions: the scene where four pixels in a row are clear of any overlay (text has
    // narrow gaps between its letters that are not scene enough).
    float4 sum = 0;
    float weight = 0;
    [loop] for (int k = 0; k < 16; ++k) {
        const float angle = k * 0.392699;
        const float2 dir = float2(cos(angle), sin(angle));
        int clear = 0;
        [loop] for (int step = 1; step <= 48; ++step) {
            const int2 q = p + int2(round(dir * step));
            if (any(q < 0) || any(q > limit)) break;
            clear = HudMaskDilated(score, q, limit) <= 0 ? clear + 1 : 0;
            if (clear == 4) {
                const float w = 1.0 / step;
                sum += currentColor[q] * w;
                weight += w;
                break;
            }
        }
    }
    hudless[p] = weight > 0 ? lerp(colour, sum / weight, m) : colour;
}
