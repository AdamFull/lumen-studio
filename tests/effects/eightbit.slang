// An effect that asks for an 8-bit texture: refused, effects work in half floats.
import effect;

struct Params
{
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba8")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;

[Effect("Eight bit", "Test")]
[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    result[id.xy] = source[id.xy];
}
