// Fork, partage (GuestStreamer.h): the presented picture, scaled to the guest's stream size
// and converted to BT.709 limited-range NV12 for NVENC. Each output pixel averages four
// bilinear taps a quarter pixel apart, a 2x2 box at 2:1 and a smooth filter below it.
// HDR outputs are brought to SDR: scRGB is clipped at its SDR white, HDR10 (PQ) is decoded,
// tone mapped from a 203 nit reference and gamma encoded.
#include "HdrColor.hlsli"
Texture2D<float4> rgb : register(t0);
RWTexture2D<float> yPlane : register(u0);
RWTexture2D<float2> uvPlane : register(u1);
SamplerState linearClamp : register(s1);
// outW, outH, hdrMode (0 SDR, 1 scRGB, 2 HDR10), ditherStep (asfloat)
cbuffer Constants : register(b0) { uint outW; uint outH; uint hdrMode; uint ditherBits; float4 padding; };

float3 toSdr(float3 c) {
    if (hdrMode == 0) return saturate(c);
    if (hdrMode == 2) c = HdrDecodePq(c);           // scRGB units, 1 = 80 cd/m2
    c = max(c * (80.0 / 203.0), 0);                  // 203 cd/m2 reference white -> 1
    // Rolls off above SDR white instead of clipping.
    c = c / (1 + max(c - 0.8, 0) * 1.25);
    return pow(saturate(c), 1 / 2.2);
}
float3 tap(float2 at) {
    const float2 step = 0.25 / float2(outW, outH);
    float3 c = rgb.SampleLevel(linearClamp, at + float2(-step.x, -step.y), 0).rgb
             + rgb.SampleLevel(linearClamp, at + float2(step.x, -step.y), 0).rgb
             + rgb.SampleLevel(linearClamp, at + float2(-step.x, step.y), 0).rgb
             + rgb.SampleLevel(linearClamp, at + float2(step.x, step.y), 0).rgb;
    return toSdr(c * 0.25);
}
float3 yuv(float3 c) {
    float y = dot(c, float3(0.2126, 0.7152, 0.0722));
    return float3((16 + 219 * y) / 255.0, (128 + 224 * (c.b - y) / 1.8556) / 255.0, (128 + 224 * (c.r - y) / 1.5748) / 255.0);
}
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outW || id.y >= outH) return;
    const float step = asfloat(ditherBits);
    const float2 size = float2(outW, outH);
    const float3 c = tap((float2(id.xy) + 0.5) / size);
    yPlane[id.xy] = yuv(c).x + OutputDither(id.xy, step);
    if ((id.x & 1) == 0 && (id.y & 1) == 0) {
        const float3 avg = (c + tap((float2(id.xy + uint2(1, 0)) + 0.5) / size)
                              + tap((float2(id.xy + uint2(0, 1)) + 0.5) / size)
                              + tap((float2(id.xy + uint2(1, 1)) + 0.5) / size)) * 0.25;
        uvPlane[id.xy / 2] = yuv(avg).yz + OutputDither(id.xy / 2, step);
    }
}
