// Detail (plan M20 step 5): sharpening and noise reduction, in the picture's own pixels (they are
// about pixels, so the preview at screen size shows them weaker than the full size does).
// Sharpening: an unsharp mask on the lightness, Radius its blur, Masking keeping it off flat parts.
// Noise reduction: Luminance smooths the lightness where it differs little (a bilateral filter, so
// edges stay), Colour takes the colour from a blurred copy where it differs little (colour blotches).
import effect;
import colorspace;
import sampling;

struct Params
{
    [Section("Sharpening")] [Label("Amount")] [Range(0, 150)] float sharpen;
    [Label("Radius")] [Range(0.5, 3)] [Default(1)] float radius;
    [Label("Masking")] [Range(0, 100)] float masking;
    [Section("Noise reduction")] [Label("Luminance")] [Range(0, 100)] float luminanceNoise;
    [Label("Colour")] [Range(0, 100)] float colourNoise;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;
// r: the lightness blurred by Radius.
[[vk::binding(2)]] [format("rgba16f")] RWTexture2D<float4> blurred;
[[vk::binding(3)]] [format("rgba16f")] RWTexture2D<float4> between;
// OKLab, a quarter of the size, blurred: the colour without its blotches.
[Scale(4)] [[vk::binding(4)]] [format("rgba16f")] RWTexture2D<float4> colour;
[Scale(4)] [[vk::binding(5)]] [format("rgba16f")] RWTexture2D<float4> colourBetween;

static const float kColourSigma = 2.0;

float lightness(float3 c)
{
    return toSrgb(max(luma(c), 0.0));
}

[Effect("Detail", "Detail")]
[shader("compute")]
[numthreads(16, 16, 1)]
void across(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const int radius = int(ceil(p.radius * 3.0));
    const float k = -0.5 / (p.radius * p.radius);
    float sum = 0.0;
    float weights = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const uint x = uint(clamp(int(id.x) + i, 0, int(p.size.x) - 1));
        const float w = exp(float(i * i) * k);
        sum += lightness(source[uint2(x, id.y)].rgb) * w;
        weights += w;
    }
    between[id.xy] = float4(sum / weights, 0.0, 0.0, 1.0);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void down(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    blurred[id.xy] = gaussian(between, id.xy, int2(0, 1), p.radius);
}

[Over("colour")]
[shader("compute")]
[numthreads(16, 16, 1)]
void shrinkColour(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    uint2 first, count;
    blockOf(source, id.xy, p.size, first, count);
    float3 sum = 0.0;
    for (uint y = 0; y < count.y; ++y) {
        for (uint x = 0; x < count.x; ++x)
            sum += toOklab(max(source[first + uint2(x, y)].rgb, 0.0));
    }
    colour[id.xy] = float4(sum / float(count.x * count.y), 1.0);
}

[Over("colour")]
[shader("compute")]
[numthreads(16, 16, 1)]
void colourAcross(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    colourBetween[id.xy] = gaussian(colour, id.xy, int2(1, 0), kColourSigma);
}

[Over("colour")]
[shader("compute")]
[numthreads(16, 16, 1)]
void colourDown(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    colour[id.xy] = gaussian(colourBetween, id.xy, int2(0, 1), kColourSigma);
}

float lightnessAt(int2 at)
{
    return lightness(source[uint2(clamp(at, int2(0), int2(p.size) - 1))].rgb);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const float4 c = source[id.xy];
    float3 rgb = max(c.rgb, 0.0);
    const int2 at = int2(id.xy);
    const float seen = lightnessAt(at);
    float after = seen;

    // Luminance noise: neighbours that differ little count, edges do not.
    const float smooth = p.luminanceNoise / 100.0;
    if (smooth > 0.0) {
        const float range = 0.015 + 0.06 * smooth;
        float sum = 0.0;
        float weights = 0.0;
        for (int y = -2; y <= 2; ++y) {
            for (int x = -2; x <= 2; ++x) {
                const float l = lightnessAt(at + int2(x, y));
                const float d = (l - seen) / range;
                const float w = exp(-0.5 * float(x * x + y * y) / 2.25 - 0.5 * d * d);
                sum += l * w;
                weights += w;
            }
        }
        after = lerp(seen, sum / weights, smooth);
    }

    // Sharpening: the difference from the blurred lightness, where there is an edge enough.
    if (p.sharpen > 0.0) {
        const float gx = lightnessAt(at + int2(1, 0)) - lightnessAt(at - int2(1, 0));
        const float gy = lightnessAt(at + int2(0, 1)) - lightnessAt(at - int2(0, 1));
        const float edge = length(float2(gx, gy));
        const float threshold = p.masking / 100.0 * 0.15;
        const float keep = threshold > 0.0 ? smoothstep(threshold * 0.5, threshold * 1.5, edge) : 1.0;
        after += p.sharpen / 100.0 * (seen - blurred[id.xy].r) * keep;
    }

    const float from = toLinear(seen);
    const float to = toLinear(max(after, 0.0));
    rgb = from > 1e-4 ? rgb * (to / from) : rgb + (to - from);

    // Colour noise: the colour of the blurred copy, unless it differs much (an edge of colour).
    const float calm = p.colourNoise / 100.0;
    if (calm > 0.0) {
        float3 lab = toOklab(rgb);
        const float2 quiet = bilinearAt(colour, id.xy, p.size).yz;
        const float2 off = lab.yz - quiet;
        lab.yz = lerp(lab.yz, quiet, calm * (1.0 - smoothstep(0.03, 0.12, length(off))));
        rgb = max(fromOklab(lab), 0.0);
    }
    result[id.xy] = float4(rgb, c.a);
}
