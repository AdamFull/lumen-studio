// Colour grading (plan M20 step 4): a tint and a lightness for the shadows, the midtones and the
// highlights, as Lightroom's colour wheels. Blending says how much they overlap, balance moves the
// line between shadows and highlights. In OKLab.
import effect;
import colorspace;

struct Params
{
    [Section("Shadows")] [Label("Shadows")] [Control("wheel")] float2 shadows; // hue in degrees, saturation in %
    [Label("Lightness")] [Range(-100, 100)] float shadowLightness;
    [Section("Midtones")] [Label("Midtones")] [Control("wheel")] float2 midtones;
    [Label("Lightness")] [Range(-100, 100)] float midtoneLightness;
    [Section("Highlights")] [Label("Highlights")] [Control("wheel")] float2 highlights;
    [Label("Lightness")] [Range(-100, 100)] float highlightLightness;
    [Section("Blend")] [Label("Blending")] [Range(0, 100)] [Default(50)] float blending;
    [Label("Balance")] [Range(-100, 100)] float balance;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;

float2 tint(float2 wheel)
{
    return wheelDirection(wheel.x) * wheel.y / 100.0 * 0.06;
}

[Effect("Colour grading", "Colour")]
[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const float4 c = source[id.xy];
    float3 lab = toOklab(max(c.rgb, 0.0));

    // From the shadows (0) to the highlights (1) around a middle that Balance moves; Blending widens
    // the way between them.
    const float middle = 0.5 - 0.25 * p.balance / 100.0;
    const float width = 0.08 + 0.42 * p.blending / 100.0;
    const float t = smoothstep(middle - width, middle + width, saturate(lab.x));
    const float shadows = 1.0 - t;
    const float highlights = t;
    const float midtones = 4.0 * t * (1.0 - t);

    lab.yz += tint(p.shadows) * shadows + tint(p.midtones) * midtones + tint(p.highlights) * highlights;
    lab.x += (p.shadowLightness * shadows + p.midtoneLightness * midtones + p.highlightLightness * highlights) / 100.0 * 0.1;
    result[id.xy] = float4(max(fromOklab(lab), 0.0), c.a);
}
