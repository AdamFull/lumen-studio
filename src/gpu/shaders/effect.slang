// What an effect says about itself (plan M20 step 2). An effect is a Slang file:
//
//   import effect;
//
//   struct Params
//   {
//       [Label("Exposure")] [Range(-5, 5)] [Default(0)] [Unit("stops")] float exposure;
//       uint2 size; // no label: not shown; Lumen fills in "size"
//   };
//   [[vk::push_constant]] ConstantBuffer<Params> p;
//   [[vk::binding(0)]] [format("rgba16f")] RWTexture2D<float4> source; // the picture so far
//   [[vk::binding(1)]] [format("rgba16f")] RWTexture2D<float4> result; // what this effect makes
//
//   [Effect("Exposure", "Light")]
//   [shader("compute")] [numthreads(16, 16, 1)]
//   void main(uint3 id: SV_DispatchThreadID) { ... }
//
// Its compute entries are its passes, in order. Pictures are in linear light, RGBA half floats with
// straight alpha. A binding named neither "source" nor "result" is a texture the effect keeps between
// its passes (a blur across, then down): the picture's size, or smaller with [Scale], [Fit] or [Size]; a
// pass with [Over] runs over one of them. All passes share the parameters; "size" is the size of
// what the pass runs over.

// The effect's name and its group in the Develop panel; on the first pass.
[__AttributeUsage(_AttributeTargets.Function)]
struct EffectAttribute
{
    string title;
    string group;
};

// A parameter's name on its slider. Parameters without one are not shown.
[__AttributeUsage(_AttributeTargets.Var)]
struct LabelAttribute
{
    string text;
};

[__AttributeUsage(_AttributeTargets.Var)]
struct RangeAttribute
{
    float minimum;
    float maximum;
};

[__AttributeUsage(_AttributeTargets.Var)]
struct DefaultAttribute
{
    float value;
};

[__AttributeUsage(_AttributeTargets.Var)]
struct UnitAttribute
{
    string text;
};

// How a parameter is set when it is not a slider: "wheel", a float2 of hue (degrees) and saturation
// (%), set on a colour wheel.
[__AttributeUsage(_AttributeTargets.Var)]
struct ControlAttribute
{
    string kind;
};

// A texture the effect reads besides the picture, with the step: "curve" (256 × 1, the curves for
// red, green, blue and all three in RGBA) or "lut" (a 3D LUT of side N as an N² × N picture).
[__AttributeUsage(_AttributeTargets.Var)]
struct InputAttribute
{
    string kind;
};

// A texture the effect keeps that is n times smaller than the picture (each side rounded up): blurs
// over a large part of the picture run fast on a small copy, and scale with the picture, so a preview
// at screen size looks like the full size.
[__AttributeUsage(_AttributeTargets.Var)]
struct ScaleAttribute
{
    float divisor;
};

// A texture the effect keeps whose long side is n pixels (the picture's shape; never larger than the
// picture). A blur of so many of its pixels covers the same part of any picture, so the preview at
// screen size and the full size agree: what Clarity and Dehaze blur over.
[__AttributeUsage(_AttributeTargets.Var)]
struct FitAttribute
{
    int pixels;
};

// A texture the effect keeps of a fixed size (Size(1, 1) for a value found over the whole picture).
[__AttributeUsage(_AttributeTargets.Var)]
struct SizeAttribute
{
    int width;
    int height;
};

// A pass that runs over one of the effect's textures instead of the picture (one thread per pixel of
// it; "size" is then that texture's size). Read the picture's size with source.GetDimensions.
[__AttributeUsage(_AttributeTargets.Function)]
struct OverAttribute
{
    string texture;
};

// A heading the parameter comes under ("White balance", "Tone"); it holds for the parameters after
// it until the next one.
[__AttributeUsage(_AttributeTargets.Var)]
struct SectionAttribute
{
    string text;
};
