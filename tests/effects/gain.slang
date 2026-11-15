// A one-pass test effect: a gain and a lift, with labels, a range, defaults, a unit and a hidden size.
import effect;

struct Params
{
    [Label("Gain")] [Range(0, 4)] [Default(1)] [Unit("×")] float gain;
    [Label("Lift")] [Range(-1, 1)] float3 lift;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;

[Effect("Gain", "Test")]
[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const float4 c = source[id.xy];
    result[id.xy] = float4(c.rgb * p.gain + p.lift, c.a);
}
