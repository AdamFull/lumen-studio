// Tone curve (plan M20 step 4): a curve for all three channels, then one per channel, on display
// values. The curves come as a 256 × 1 half-float picture: red, green and blue hold each channel's
// curve, alpha the one for all.
import effect;
import colorspace;

struct Params
{
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;
[Input("curve")] [[vk::binding(2)]] [format("rgba16f")] RWTexture2D<float4> curve;

// The curve at v (0..1), between its 256 points; above 1 it goes on with its slope at the top.
float4 lookup(float v)
{
    const float x = saturate(v) * 255.0;
    const uint i = min(uint(x), 254u);
    const float4 a = curve[uint2(i, 0)];
    const float4 b = curve[uint2(i + 1, 0)];
    float4 y = lerp(a, b, x - float(i));
    if (v > 1.0)
        y += (b - a) * 255.0 * (v - 1.0);
    return y;
}

[Effect("Tone curve", "Tone curve")]
[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const float4 c = source[id.xy];
    float3 v = toSrgb(max(c.rgb, 0.0));
    v = float3(lookup(v.r).a, lookup(v.g).a, lookup(v.b).a);
    v = float3(lookup(v.r).r, lookup(v.g).g, lookup(v.b).b);
    result[id.xy] = float4(toLinear(max(v, 0.0)), c.a);
}
