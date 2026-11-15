// Colour conversions effects share (`import colorspace;`). Pictures are in linear light, sRGB
// primaries.
module colorspace;

// The sRGB curve, for what is done on display values (curves, LUTs). Values above 1 go on in a
// straight line, so bright light is not cut off.
public float toSrgb(float c)
{
    if (c > 1.0)
        return 1.0 + (c - 1.0) * (1.055 / 2.4);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
}

public float toLinear(float c)
{
    if (c > 1.0)
        return 1.0 + (c - 1.0) * (2.4 / 1.055);
    return c <= 0.04045 ? c / 12.92 : pow((max(c, 0.0) + 0.055) / 1.055, 2.4);
}

public float3 toSrgb(float3 c)
{
    return float3(toSrgb(c.r), toSrgb(c.g), toSrgb(c.b));
}

public float3 toLinear(float3 c)
{
    return float3(toLinear(c.r), toLinear(c.g), toLinear(c.b));
}

public float luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float cubeRoot(float x)
{
    return sign(x) * pow(abs(x), 1.0 / 3.0);
}

// OKLab (Björn Ottosson): lightness and two colour axes, spaced as people see them. Its hue is the
// angle of (a, b).
public float3 toOklab(float3 c)
{
    const float l = cubeRoot(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b);
    const float m = cubeRoot(0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b);
    const float s = cubeRoot(0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    return float3(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                  1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                  0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s);
}

public float3 fromOklab(float3 c)
{
    float l = c.x + 0.3963377774 * c.y + 0.2158037573 * c.z;
    float m = c.x - 0.1055613458 * c.y - 0.0638541728 * c.z;
    float s = c.x - 0.0894841775 * c.y - 1.2914855480 * c.z;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    return float3(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                  -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                  -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}

// The direction in OKLab's (a, b) of the colour at `degrees` on a colour wheel (HSV hue, as Lumen's
// wheels draw it), so a tint is the colour the wheel shows.
public float2 wheelDirection(float degrees)
{
    const float h = degrees / 60.0;
    const float3 hsv = saturate(float3(abs(h - 3.0) - 1.0, 2.0 - abs(h - 2.0), 2.0 - abs(h - 4.0)));
    const float2 ab = toOklab(toLinear(hsv)).yz;
    return ab / max(length(ab), 1e-6);
}
