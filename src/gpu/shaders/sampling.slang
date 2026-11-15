// Reading textures between sizes, which effects with smaller textures of their own share
// (`import sampling;`): a box average down, a Gaussian blur within one, and a smooth read back up.
module sampling;

uint2 dimensions(RWTexture2D<float4> t)
{
    uint w, h;
    t.GetDimensions(w, h);
    return uint2(max(w, 1u), max(h, 1u));
}

// The texture between its pixels: `at` in its pixels, their centres at .5; the edges held.
public float4 bilinear(RWTexture2D<float4> t, float2 at)
{
    const uint2 size = dimensions(t);
    const float2 p = clamp(at - 0.5, float2(0.0), float2(size - 1u));
    const uint2 i = uint2(p);
    const uint2 j = min(i + 1u, size - 1u);
    const float2 f = p - float2(i);
    return lerp(lerp(t[i], t[uint2(j.x, i.y)], f.x), lerp(t[uint2(i.x, j.y)], t[j], f.x), f.y);
}

// What a smaller (or larger) texture holds at a pixel of something `size` big: the same place.
public float4 bilinearAt(RWTexture2D<float4> t, uint2 pixel, uint2 size)
{
    return bilinear(t, (float2(pixel) + 0.5) * float2(dimensions(t)) / float2(max(size, 1u)));
}

// The pixels of `from` that fall into pixel `at` of a texture `size` big, how many and where they
// start: at least one, so a texture larger than `from` takes the nearest.
public void blockOf(RWTexture2D<float4> from, uint2 at, uint2 size, out uint2 first, out uint2 count)
{
    const uint2 big = dimensions(from);
    first = min(uint2(float2(at) * float2(big) / float2(size)), big - 1u);
    const uint2 end = min(uint2(float2(at + 1u) * float2(big) / float2(size)), big);
    count = max(end, first + 1u) - first;
}

// The average of `from` over that block.
public float4 boxDown(RWTexture2D<float4> from, uint2 at, uint2 size)
{
    uint2 first, count;
    blockOf(from, at, size, first, count);
    float4 sum = 0.0;
    for (uint y = 0; y < count.y; ++y) {
        for (uint x = 0; x < count.x; ++x)
            sum += from[first + uint2(x, y)];
    }
    return sum / float(count.x * count.y);
}

// A Gaussian of `sigma` pixels along one direction ((1, 0) across, (0, 1) down), the edges held: run
// it across into one texture and down into another for a blur both ways.
public float4 gaussian(RWTexture2D<float4> t, uint2 at, int2 direction, float sigma)
{
    const int2 size = int2(dimensions(t));
    if (sigma < 0.3)
        return t[at];
    const int radius = int(ceil(sigma * 3.0));
    const float k = -0.5 / (sigma * sigma);
    float4 sum = 0.0;
    float weights = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const int2 p = clamp(int2(at) + direction * i, int2(0), size - 1);
        const float w = exp(float(i * i) * k);
        sum += t[uint2(p)] * w;
        weights += w;
    }
    return sum / weights;
}
