// The Develop panel's basics (plan M20 steps 3 and 9): white balance, exposure and tone, presence,
// in linear light. All at 0 leaves the picture as it is.
//
// Highlights and Shadows are local: they move an area by the light around it (a blurred copy of the
// lightness, "around"), so an area's texture moves with it and keeps its contrast. Where a pixel is
// far from the light around it (an edge) it goes by its own light instead, so there are no halos.
// The copy's long side is fixed, so the preview at screen size looks like the full size.
import effect;
import sampling;

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
// r: the lightness around each place (after exposure, before the tone sliders).
[Fit(256)] [[vk::binding(2)]] [format("rgba16f")] RWTexture2D<float4> around;
[Fit(256)] [[vk::binding(3)]] [format("rgba16f")] RWTexture2D<float4> aroundBetween;

static const float kAroundSigma = 2.0;
// How far (on the perceptual scale) a pixel may be from the light around it and still move with it.
static const float kEdge = 0.12;

float luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Lightness on a perceptual scale (about what 8-bit values are), where the tone sliders work.
float perceptual(float y)
{
    return pow(max(y, 0.0), 1.0 / 2.2);
}

// The inverse of smoothstep on [0, 1]: lower contrast with the same black and white.
float inverseSmoothstep(float v)
{
    return 0.5 - sin(asin(1.0 - 2.0 * v) / 3.0);
}

// Contrast: an S-curve around the middle, or its inverse; black and white stay where they are.
float contrasted(float v)
{
    const float c = p.contrast / 100.0;
    const float clamped = saturate(v);
    const float s = c >= 0.0 ? lerp(clamped, clamped * clamped * (3.0 - 2.0 * clamped), c)
                             : lerp(clamped, inverseSmoothstep(clamped), -c);
    return v + s - clamped;
}

// The tone sliders. Each moves its part of the range and fades out towards the others; Highlights
// and Shadows by `guide`, the light that steers them, the others by the pixel's own.
float tone(float v, float guide)
{
    v += p.blacks / 100.0 * 0.15 * (1.0 - smoothstep(0.0, 0.35, v));
    // Shadows from a little above black: the darkest stay dark (Blacks moves those).
    v += p.shadows / 100.0 * 0.25 * smoothstep(0.04, 0.24, guide) * (1.0 - smoothstep(0.24, 0.58, guide));
    v += p.highlights / 100.0 * 0.25 * smoothstep(0.45, 0.75, guide) * (1.0 - smoothstep(0.85, 1.05, guide));
    v += p.whites / 100.0 * 0.15 * smoothstep(0.65, 1.0, v);
    return max(v, 0.0);
}

// The light around: the lightness after exposure (white balance keeps it), averaged down.
[Effect("Develop", "Basic")]
[Over("around")]
[shader("compute")]
[numthreads(16, 16, 1)]
void shrink(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    uint2 first, count;
    blockOf(source, id.xy, p.size, first, count);
    const float gain = exp2(p.exposure);
    float sum = 0.0;
    for (uint y = 0; y < count.y; ++y) {
        for (uint x = 0; x < count.x; ++x)
            sum += perceptual(luma(source[first + uint2(x, y)].rgb) * gain);
    }
    around[id.xy] = float4(sum / float(count.x * count.y), 0.0, 0.0, 1.0);
}

[Over("around")]
[shader("compute")]
[numthreads(16, 16, 1)]
void across(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    aroundBetween[id.xy] = gaussian(around, id.xy, int2(1, 0), kAroundSigma);
}

[Over("around")]
[shader("compute")]
[numthreads(16, 16, 1)]
void down(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    around[id.xy] = gaussian(aroundBetween, id.xy, int2(0, 1), kAroundSigma);
}

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
    const float v = contrasted(perceptual(y));
    // What steers Highlights and Shadows: the light around where the pixel is like it, the pixel's
    // own where it is not (an edge).
    float guide = v;
    if (p.highlights != 0.0 || p.shadows != 0.0) {
        const float near = contrasted(bilinearAt(around, id.xy, p.size).r);
        const float d = (v - near) / kEdge;
        guide = lerp(v, near, exp(-d * d));
    }
    const float toned = pow(tone(v, guide), 2.2);
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
