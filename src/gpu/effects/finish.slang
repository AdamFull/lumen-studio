// Vignette and grain (plan M20 step 5), the last touches. The vignette is measured on the picture's
// shape, so the preview at screen size looks like the full size; grain is in pixels (see below).
// Vignette: darker (or lighter) towards the edges. Midpoint is where it starts, Roundness how round
// it is (0 follows the picture's shape, 100 a circle, below 0 nearer a rectangle), Feather how soft.
// Grain: film-like noise on the lightness; Size the grains' size, Roughness how uneven they are.
import effect;
import colorspace;

struct Params
{
    [Section("Vignette")] [Label("Amount")] [Range(-100, 100)] float vignette;
    [Label("Midpoint")] [Range(0, 100)] [Default(50)] float midpoint;
    [Label("Roundness")] [Range(-100, 100)] float roundness;
    [Label("Feather")] [Range(0, 100)] [Default(50)] float feather;
    [Section("Grain")] [Label("Amount")] [Range(0, 100)] float grain;
    [Label("Size")] [Range(0, 100)] [Default(25)] float grainSize;
    [Label("Roughness")] [Range(0, 100)] [Default(50)] float roughness;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;

// 0 in the middle, 1 where the vignette is at its full strength.
float vignetteAt(float2 pixel)
{
    const float2 size = float2(p.size);
    float2 q = pixel / size * 2.0 - 1.0;
    const float roundness = p.roundness / 100.0;
    if (roundness > 0.0)
        q *= lerp(float2(1.0), size / max(size.x, size.y), roundness);
    const float power = roundness < 0.0 ? 2.0 - 6.0 * roundness : 2.0;
    const float d = pow(pow(abs(q.x), power) + pow(abs(q.y), power), 1.0 / power);
    const float start = lerp(0.15, 1.25, p.midpoint / 100.0);
    const float width = lerp(0.03, 1.2, p.feather / 100.0);
    return smoothstep(start, start + width, d);
}

uint hash(uint2 v)
{
    // PCG over the two coordinates.
    uint s = v.x * 747796405u + v.y * 2891336453u + 12345u;
    s = s * 747796405u + 2891336453u;
    s = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (s >> 22u) ^ s;
}

// A cell's random value from -1 to 1.
float cellValue(int2 cell)
{
    return float(hash(uint2(cell + 65536))) / 2147483647.5 - 1.0;
}

// Smooth noise from -1 to 1 with cells of one unit.
float noise(float2 at)
{
    const float2 i = floor(at);
    const float2 f = at - i;
    const float2 u = f * f * (3.0 - 2.0 * f);
    const int2 c = int2(i);
    return lerp(lerp(cellValue(c), cellValue(c + int2(1, 0)), u.x), lerp(cellValue(c + int2(0, 1)), cellValue(c + int2(1, 1)), u.x), u.y);
}

[Effect("Vignette and grain", "Effects")]
[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const float4 c = source[id.xy];
    float3 rgb = max(c.rgb, 0.0);
    const float2 pixel = float2(id.xy) + 0.5;

    const float amount = p.vignette / 100.0;
    if (amount != 0.0) {
        const float v = vignetteAt(pixel);
        rgb = amount < 0.0 ? rgb * exp2(2.5 * amount * v) : toLinear(lerp(toSrgb(rgb), float3(1.0), 0.7 * amount * v));
    }

    if (p.grain > 0.0) {
        // Grains of a pixel or a few; on pictures over 2000 pixels they grow with the picture, as
        // film grain does with the frame, so a preview at screen size looks about like the full size.
        const float longSide = float(max(p.size.x, p.size.y));
        const float cell = (1.0 + 3.0 * p.grainSize / 100.0) * max(1.0, longSide / 2000.0);
        const float2 at = pixel / cell;
        float n = noise(at);
        n = lerp(n, 0.6 * n + 0.6 * noise(at * 2.7 + 17.0), p.roughness / 100.0);
        const float strength = p.grain / 100.0 * 0.12 * min(cell, 1.0);
        const float before = toSrgb(max(luma(rgb), 0.0));
        const float after = max(before + strength * n * (0.25 + 3.0 * before * (1.0 - before)), 0.0);
        const float from = toLinear(before);
        const float to = toLinear(after);
        rgb = from > 1e-4 ? rgb * (to / from) : rgb + (to - from);
    }
    result[id.xy] = float4(rgb, c.a);
}
