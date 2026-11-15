// Local contrast (plan M20 step 5): Texture, Clarity and Dehaze, each against a blurred copy of the
// picture. The copies are small textures whose long side is fixed, so they blur the same part of any
// picture and the preview at screen size looks like the full size: "fine" (1024 pixels) for Texture,
// "coarse" (256) for Clarity and Dehaze. Each detail is taken on the lightness as people see it and
// put back on the colour by its light; large steps (edges) are held back so they do not glow.
import effect;
import colorspace;
import sampling;

struct Params
{
    [Label("Texture")] [Range(-100, 100)] float texture;
    [Label("Clarity")] [Range(-100, 100)] float clarity;
    [Label("Dehaze")] [Range(-100, 100)] float dehaze;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;
// rgb: the light; a: the lightness as seen (sRGB curve over the luminance), each averaged.
[Fit(1024)] [[vk::binding(2)]] [format("rgba16f")] RWTexture2D<float4> fine;
[Fit(1024)] [[vk::binding(3)]] [format("rgba16f")] RWTexture2D<float4> fineBetween;
[Fit(256)] [[vk::binding(4)]] [format("rgba16f")] RWTexture2D<float4> coarse;
[Fit(256)] [[vk::binding(5)]] [format("rgba16f")] RWTexture2D<float4> coarseBetween;
// The haze's colour: the brightest of the coarse copy.
[Size(1, 1)] [[vk::binding(6)]] [format("rgba16f")] RWTexture2D<float4> air;

static const float kFineSigma = 1.5;
static const float kCoarseSigma = 4.0;

float lightness(float3 c)
{
    return toSrgb(max(luma(c), 0.0));
}

uint2 pictureSize()
{
    uint w, h;
    source.GetDimensions(w, h);
    return uint2(w, h);
}

[Effect("Local contrast", "Presence")]
[Over("fine")]
[shader("compute")]
[numthreads(16, 16, 1)]
void shrinkFine(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    uint2 first, count;
    blockOf(source, id.xy, p.size, first, count);
    float4 sum = 0.0;
    for (uint y = 0; y < count.y; ++y) {
        for (uint x = 0; x < count.x; ++x) {
            const float3 c = max(source[first + uint2(x, y)].rgb, 0.0);
            sum += float4(c, lightness(c));
        }
    }
    fine[id.xy] = sum / float(count.x * count.y);
}

[Over("fine")]
[shader("compute")]
[numthreads(16, 16, 1)]
void fineAcross(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    fineBetween[id.xy] = gaussian(fine, id.xy, int2(1, 0), kFineSigma);
}

[Over("fine")]
[shader("compute")]
[numthreads(16, 16, 1)]
void fineDown(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    fine[id.xy] = gaussian(fineBetween, id.xy, int2(0, 1), kFineSigma);
}

[Over("coarse")]
[shader("compute")]
[numthreads(16, 16, 1)]
void shrinkCoarse(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    coarse[id.xy] = boxDown(fine, id.xy, p.size);
}

[Over("coarse")]
[shader("compute")]
[numthreads(16, 16, 1)]
void coarseAcross(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    coarseBetween[id.xy] = gaussian(coarse, id.xy, int2(1, 0), kCoarseSigma);
}

[Over("coarse")]
[shader("compute")]
[numthreads(16, 16, 1)]
void coarseDown(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    coarse[id.xy] = gaussian(coarseBetween, id.xy, int2(0, 1), kCoarseSigma);
}

// One thread: the haze's colour is the brightest part of the blurred picture.
[Over("air")]
[shader("compute")]
[numthreads(1, 1, 1)]
void findAir(uint3 id: SV_DispatchThreadID)
{
    uint w, h;
    coarse.GetDimensions(w, h);
    float4 best = coarse[uint2(0, 0)];
    for (uint y = 0; y < h; ++y) {
        for (uint x = 0; x < w; ++x) {
            const float4 c = coarse[uint2(x, y)];
            if (c.a > best.a)
                best = c;
        }
    }
    air[uint2(0, 0)] = best;
}

// A detail with its large steps held back: small ones as they are, large ones less.
float soft(float d)
{
    return d / (1.0 + 3.0 * abs(d));
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const float4 c = source[id.xy];
    float3 rgb = max(c.rgb, 0.0);
    const float4 big = bilinearAt(coarse, id.xy, p.size);
    // The details, from the picture as it came (the copies are of it, not of the dehazed one).
    const float seen = lightness(rgb);
    const float small = soft(seen - bilinearAt(fine, id.xy, p.size).a);
    const float large = soft(seen - big.a);

    // Dehaze: the haze is how much of the air's colour the darkest channel nearby holds
    // (He, Sun and Tang's dark channel, over the coarse copy); taken out, or added for less than 0.
    const float amount = p.dehaze / 100.0;
    // The haze's colour: mostly a grey as light as the brightest part, so a coloured light (a
    // setting sun) does not tint the whole picture its opposite when taken out.
    const float3 brightest = max(air[uint2(0, 0)].rgb, 0.0);
    const float3 haze = lerp(float3(luma(brightest)), brightest, 0.25);
    if (amount > 0.0) {
        const float3 a = max(haze, 1e-3);
        // Never more haze than the pixel itself could hold: a blurred estimate alone spills over
        // bright edges (a ring round the sun).
        const float nearby = min(min(big.r / a.r, big.g / a.g), big.b / a.b);
        const float own = min(min(rgb.r / a.r, rgb.g / a.g), rgb.b / a.b);
        const float dark = min(nearby, own);
        const float through = max(1.0 - 0.95 * amount * saturate(dark), 0.1);
        rgb = max((rgb - a) / through + a, 0.0);
    } else if (amount < 0.0) {
        rgb = lerp(rgb, haze, -amount * 0.45);
    }

    // Texture against the fine copy, Clarity against the coarse one, mostly in the midtones.
    const float before = lightness(rgb);
    const float middle = 1.0 - 0.7 * (2.0 * saturate(before) - 1.0) * (2.0 * saturate(before) - 1.0);
    const float after = before + p.texture / 100.0 * small + p.clarity / 100.0 * 0.8 * middle * large;

    const float from = toLinear(before);
    const float to = toLinear(max(after, 0.0));
    rgb = from > 1e-4 ? rgb * (to / from) : rgb + (to - from);
    result[id.xy] = float4(rgb, c.a);
}
