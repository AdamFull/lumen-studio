// A look from a 3D LUT (plan M20 step 4; .cube files), on display values, mixed in by Amount. The LUT
// comes as an N² × N half-float picture: blue picks the column block, red the column in it, green
// the row.
import effect;
import colorspace;

struct Params
{
    [Label("Amount")] [Range(0, 100)] [Default(100)] float amount;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;
[Input("lut")] [[vk::binding(2)]] [format("rgba16f")] RWTexture2D<float4> lut;

float3 entry(uint3 at, uint n)
{
    return lut[uint2(at.r + at.b * n, at.g)].rgb;
}

[Effect("LUT", "Look")]
[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    uint width, height;
    lut.GetDimensions(width, height);
    const uint n = max(height, 2u);
    const float4 c = source[id.xy];
    const float3 where = saturate(toSrgb(max(c.rgb, 0.0))) * float(n - 1);
    const uint3 lo = min(uint3(where), uint3(n - 2, n - 2, n - 2));
    const float3 f = where - float3(lo);
    // Trilinear between the eight entries around the colour.
    const float3 c00 = lerp(entry(lo, n), entry(lo + uint3(1, 0, 0), n), f.r);
    const float3 c10 = lerp(entry(lo + uint3(0, 1, 0), n), entry(lo + uint3(1, 1, 0), n), f.r);
    const float3 c01 = lerp(entry(lo + uint3(0, 0, 1), n), entry(lo + uint3(1, 0, 1), n), f.r);
    const float3 c11 = lerp(entry(lo + uint3(0, 1, 1), n), entry(lo + uint3(1, 1, 1), n), f.r);
    const float3 looked = lerp(lerp(c00, c10, f.g), lerp(c01, c11, f.g), f.b);
    result[id.xy] = float4(lerp(c.rgb, toLinear(looked), p.amount / 100.0), c.a);
}
