// Counts the luma samples of a decoded stream picture that differ from the picture
// before it by more than a noise band (veyra/pipeline/StreamPictureDiff.h).
//
// The count is added to a running total: the host subtracts the total it read for
// the previous picture, so nothing has to be cleared between pictures.
Texture2D<float> Current : register(t0);
Texture2D<float> Previous : register(t1);
RWByteAddressBuffer Total : register(u0);
cbuffer Settings : register(b0) {
    uint2 Size;   // visible luma extent; decoder padding is never read
    float Band;   // on the normalized sample scale
};
// Each thread compares a 4x4 block, so a 1080p picture is 60x34 groups.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint2 origin = id.xy * 4;
    uint count = 0;
    for (uint y = 0; y < 4; ++y)
        for (uint x = 0; x < 4; ++x) {
            const uint2 p = origin + uint2(x, y);
            if (p.x < Size.x && p.y < Size.y)
                count += abs(Current.Load(int3(p, 0)) - Previous.Load(int3(p, 0))) > Band ? 1 : 0;
        }
    if (count) {
        uint before;
        Total.InterlockedAdd(0, count, before);
    }
}
