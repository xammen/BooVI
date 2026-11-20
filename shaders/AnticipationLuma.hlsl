// Camera anticipation: a 160x90 luma copy of the picture on the stage. Its column and row
// profiles, lined up against the previous picture's, give the picture's global motion
// (veyra/engine/CameraAnticipation.h). 36 taps a cell stand in for the mips the page used.
Texture2D<float4> Source : register(t0);
RWTexture2D<float> Luma : register(u0);
SamplerState pointClamp : register(s0);
SamplerState linearClamp : register(s1);
cbuffer Settings : register(b0) {
    float2 Cell;    // one output cell in source uv
    float Linear;   // 1: linear scRGB, compressed so that highlights do not own the profile
    float Pad0;
    float4 Pad1;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= 160 || id.y >= 90) return;
    const float2 origin = float2(id.xy) * Cell;
    float sum = 0;
    for (uint y = 0; y < 6; ++y)
        for (uint x = 0; x < 6; ++x) {
            const float3 c = Source.SampleLevel(linearClamp, origin + (float2(x, y) + 0.5) / 6.0 * Cell, 0).rgb;
            const float l = dot(max(c, 0), float3(0.3, 0.59, 0.11));
            sum += Linear > 0.5 ? sqrt(min(l, 16.0)) * 0.25 : l;
        }
    Luma[id.xy] = sum / 36.0;
}
