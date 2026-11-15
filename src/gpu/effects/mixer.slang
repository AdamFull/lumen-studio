// Colour mixer (plan M20 step 4), Lightroom's HSL: hue, saturation and luminance for eight colour
// bands, in OKLab so a band's colours are moved as people see them. A colour between two bands takes
// from both.
import effect;
import colorspace;

struct Params
{
    [Section("Hue")] [Label("Red")] [Range(-100, 100)] float hueRed;
    [Label("Orange")] [Range(-100, 100)] float hueOrange;
    [Label("Yellow")] [Range(-100, 100)] float hueYellow;
    [Label("Green")] [Range(-100, 100)] float hueGreen;
    [Label("Aqua")] [Range(-100, 100)] float hueAqua;
    [Label("Blue")] [Range(-100, 100)] float hueBlue;
    [Label("Purple")] [Range(-100, 100)] float huePurple;
    [Label("Magenta")] [Range(-100, 100)] float hueMagenta;
    [Section("Saturation")] [Label("Red")] [Range(-100, 100)] float satRed;
    [Label("Orange")] [Range(-100, 100)] float satOrange;
    [Label("Yellow")] [Range(-100, 100)] float satYellow;
    [Label("Green")] [Range(-100, 100)] float satGreen;
    [Label("Aqua")] [Range(-100, 100)] float satAqua;
    [Label("Blue")] [Range(-100, 100)] float satBlue;
    [Label("Purple")] [Range(-100, 100)] float satPurple;
    [Label("Magenta")] [Range(-100, 100)] float satMagenta;
    [Section("Luminance")] [Label("Red")] [Range(-100, 100)] float lumRed;
    [Label("Orange")] [Range(-100, 100)] float lumOrange;
    [Label("Yellow")] [Range(-100, 100)] float lumYellow;
    [Label("Green")] [Range(-100, 100)] float lumGreen;
    [Label("Aqua")] [Range(-100, 100)] float lumAqua;
    [Label("Blue")] [Range(-100, 100)] float lumBlue;
    [Label("Purple")] [Range(-100, 100)] float lumPurple;
    [Label("Magenta")] [Range(-100, 100)] float lumMagenta;
    uint2 size;
};
[[vk::push_constant]] ConstantBuffer<Params> p;

[[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source;
[[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result;

// Where each band sits on OKLab's hue circle, in degrees.
static const float kCentres[8] = {29.0, 55.0, 105.0, 142.0, 195.0, 264.0, 300.0, 340.0};

[Effect("Colour mixer", "Colour")]
[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= p.size))
        return;
    const float4 c = source[id.xy];
    float3 lab = toOklab(max(c.rgb, 0.0));
    const float chroma = length(lab.yz);
    if (chroma < 1e-5) {
        result[id.xy] = c; // grey: no colour to move
        return;
    }
    const float hue = fmod(degrees(atan2(lab.z, lab.y)) + 360.0, 360.0);

    // The two bands the hue lies between, and how far from each.
    int below = 7;
    for (int i = 0; i < 8; ++i) {
        if (hue >= kCentres[i])
            below = i;
    }
    const int above = (below + 1) % 8;
    float span = kCentres[above] - kCentres[below];
    float along = hue - kCentres[below];
    if (span <= 0.0)
        span += 360.0;
    if (along < 0.0)
        along += 360.0;
    const float t = smoothstep(0.0, 1.0, along / span);

    const float hues[8] = {p.hueRed, p.hueOrange, p.hueYellow, p.hueGreen, p.hueAqua, p.hueBlue, p.huePurple, p.hueMagenta};
    const float sats[8] = {p.satRed, p.satOrange, p.satYellow, p.satGreen, p.satAqua, p.satBlue, p.satPurple, p.satMagenta};
    const float lums[8] = {p.lumRed, p.lumOrange, p.lumYellow, p.lumGreen, p.lumAqua, p.lumBlue, p.lumPurple, p.lumMagenta};
    const float shift = lerp(hues[below], hues[above], t) / 100.0 * 30.0;
    const float saturation = 1.0 + lerp(sats[below], sats[above], t) / 100.0;
    // Lightness moves colours, not greys: the weaker the colour, the less.
    const float lightness = lerp(lums[below], lums[above], t) / 100.0 * 0.25 * smoothstep(0.0, 0.08, chroma);

    const float angle = radians(hue + shift);
    lab.yz = float2(cos(angle), sin(angle)) * chroma * max(saturation, 0.0);
    lab.x *= 1.0 + lightness;
    result[id.xy] = float4(max(fromOklab(lab), 0.0), c.a);
}
