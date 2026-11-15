// A two-pass test effect: a box blur across into a texture of its own, then down into the result.
import effect;

struct Params
{
    [Label("Radius")] [Range(0, 8)] [Default(1)] int radius;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;
[[vk::binding(2)]] [format("rgba16f")] RWTexture2D<float4> between;

[Effect("Box blur", "Test")]
[shader("compute")]
[numthreads(16, 16, 1)]
void across(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    float4 sum = 0;
    for (int i = -p.radius; i <= p.radius; ++i)
        sum += source[uint2(clamp(int(id.x) + i, 0, int(p.size.x) - 1), id.y)];
    between[id.xy] = sum / float(2 * p.radius + 1);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void down(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    float4 sum = 0;
    for (int i = -p.radius; i <= p.radius; ++i)
        sum += between[uint2(id.x, clamp(int(id.y) + i, 0, int(p.size.y) - 1))];
    result[id.xy] = sum / float(2 * p.radius + 1);
}
