// Shared by the frame-generation guard passes (FgHudDetect/FgHudInpaint/FgFinalize).
float HudMask(float score) { return smoothstep(0.3, 0.7, score); }

// The overlay mask grown by one pixel: an overlay's anti-aliased edge blends it with the scene.
float HudMaskDilated(Texture2D<float> score, int2 p, int2 limit)
{
    float m = 0;
    [unroll] for (int j = 0; j < 9; ++j)
        m = max(m, HudMask(score[clamp(p + int2(j % 3 - 1, j / 3 - 1), int2(0, 0), limit)]));
    return m;
}

// FgStats decision bits (stats texel 1, z).
static const uint kFgHold = 1u;    // present the nearest real frame
static const uint kFgGlobal = 2u;  // blend in the global-motion interpolation where it holds
