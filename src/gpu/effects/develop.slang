// The Develop panel's basics (plan M20 step 3): white balance, exposure and tone, presence. One pass,
// pixel by pixel, in linear light. All at 0 leaves the picture as it is.
import effect;

struct Params
{
    [Section("White balance")] [Label("Temperature")] [Range(-100, 100)] float temperature;
    [Label("Tint")] [Range(-100, 100)] float tint;
    [Section("Tone")] [Label("Exposure")] [Range(-5, 5)] float exposure; // stops
    [Label("Contrast")] [Range(-100, 100)] float contrast;
    [Label("Highlights")] [Range(-100, 100)] float highlights;
    [Label("Shadows")] [Range(-100, 100)] float shadows;
    [Label("Whites")] [Range(-100, 100)] float whites;
    [Label("Blacks")] [Range(-100, 100)] float blacks;
    [Section("Presence")] [Label("Vibrance")] [Range(-100, 100)] float vibrance;
    [Label("Saturation")] [Range(-100, 100)] float saturation;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;

float luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// The inverse of smoothstep on [0, 1]: lower contrast with the same black and white.
float inverseSmoothstep(float v)
{
    return 0.5 - sin(asin(1.0 - 2.0 * v) / 3.0);
}

// Lightness on a perceptual scale (about what 8-bit values are), where the tone sliders work.
float tone(float v)
{
    // Contrast: an S-curve around the middle, or its inverse; black and white stay where they are.
    const float c = p.contrast / 100.0;
    const float clamped = saturate(v);
    const float s = c >= 0.0 ? lerp(clamped, clamped * clamped * (3.0 - 2.0 * clamped), c)
                             : lerp(clamped, inverseSmoothstep(clamped), -c);
    v += s - clamped;
    // Each slider moves its part of the range and fades out towards the others.
    v += p.blacks / 100.0 * 0.15 * (1.0 - smoothstep(0.0, 0.35, v));
    // Shadows from a little above black: the darkest stay dark (Blacks moves those).
    v += p.shadows / 100.0 * 0.25 * smoothstep(0.04, 0.24, v) * (1.0 - smoothstep(0.24, 0.58, v));
    v += p.highlights / 100.0 * 0.25 * smoothstep(0.45, 0.75, v) * (1.0 - smoothstep(0.85, 1.05, v));
    v += p.whites / 100.0 * 0.15 * smoothstep(0.65, 1.0, v);
    return max(v, 0.0);
}

[Effect("Develop", "Basic")]
[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const float4 c = source[id.xy];
    float3 x = c.rgb;

    // White balance: warmer or cooler on the blue–amber axis, tint on the green–magenta one, as bright
    // as before.
    const float t = p.temperature / 100.0;
    const float g = p.tint / 100.0;
    const float before = luma(x);
    x *= float3(exp2(0.3 * t + 0.1 * g), exp2(-0.2 * g), exp2(-0.3 * t + 0.1 * g));
    x *= before / max(luma(x), 1e-6);

    x *= exp2(p.exposure);

    // Tone on the luminance; the colour keeps its ratios.
    const float y = luma(x);
    const float toned = pow(tone(pow(max(y, 0.0), 1.0 / 2.2)), 2.2);
    x = y > 1e-6 ? x * (toned / y) : float3(toned, toned, toned);

    // Vibrance lifts the colours that are not strong yet; saturation all of them.
    const float yy = luma(x);
    const float high = max(x.r, max(x.g, x.b));
    const float low = min(x.r, min(x.g, x.b));
    const float chroma = high > 1e-6 ? (high - low) / high : 0.0;
    const float amount = (1.0 + p.vibrance / 100.0 * (1.0 - chroma)) * (1.0 + p.saturation / 100.0);
    x = max(yy + (x - yy) * amount, 0.0);

    result[id.xy] = float4(x, c.a);
}
