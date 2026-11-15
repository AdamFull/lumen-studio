// What every effect chain starts and ends with: an 8-bit sRGB picture with straight alpha, and linear
// light in half floats in between.

struct ColorParams
{
    uint2 size;
    uint seed;
    float dither; // 0: none, 1: ±half a step of 8 bits
};
[[vk::push_constant]] ConstantBuffer<ColorParams> p;

[[vk::binding(0)]] [format("rgba8")] RWTexture2D<float4> picture; // decode reads it, encode writes it
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> light; // decode writes it, encode reads it

float3 toLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

float3 toSrgb(float3 c)
{
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

uint hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void decode(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const float4 c = picture[id.xy];
    light[id.xy] = float4(toLinear(c.rgb), c.a);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void encode(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const float4 c = light[id.xy];
    const uint h = hash(id.x * 1973 + id.y * 9277 + p.seed * 26699);
    const float noise = (float(h & 0xffff) / 65535.0 - 0.5) / 255.0;
    picture[id.xy] = float4(saturate(toSrgb(c.rgb) + noise * p.dither), saturate(c.a));
}
